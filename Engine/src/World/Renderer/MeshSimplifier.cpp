#include "World/Renderer/MeshSimplifier.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace World
{
	namespace
	{
		struct SymmetricMatrix
		{
			// 对称矩阵 10 个独立分量:
			// [0] m00, [1] m01, [2] m02, [3] m03
			//          [4] m11, [5] m12, [6] m13
			//                   [7] m22, [8] m23
			//                            [9] m33
			double m[10] = { 0.0 };

			SymmetricMatrix() = default;

			SymmetricMatrix(double a, double b, double c, double d)
			{
				m[0] = a * a;  m[1] = a * b;  m[2] = a * c;  m[3] = a * d;
				               m[4] = b * b;  m[5] = b * c;  m[6] = b * d;
				                              m[7] = c * c;  m[8] = c * d;
				                                             m[9] = d * d;
			}

			SymmetricMatrix& operator+=(const SymmetricMatrix& n)
			{
				for (int i = 0; i < 10; ++i)
					m[i] += n.m[i];
				return *this;
			}

			SymmetricMatrix operator+(const SymmetricMatrix& n) const
			{
				SymmetricMatrix res = *this;
				res += n;
				return res;
			}

			double Evaluate(const glm::vec3& p) const
			{
				double x = p.x;
				double y = p.y;
				double z = p.z;
				return m[0] * x * x + 2.0 * m[1] * x * y + 2.0 * m[2] * x * z + 2.0 * m[3] * x
				     + m[4] * y * y + 2.0 * m[5] * y * z + 2.0 * m[6] * y
				     + m[7] * z * z + 2.0 * m[8] * z
				     + m[9];
			}
		};

		struct SimpTriangle
		{
			uint32_t v[3] = { 0, 0, 0 };
			glm::vec3 Normal { 0.0f };
			bool Deleted = false;
		};

		struct SimpVertex
		{
			glm::vec3 Position { 0.0f };
			glm::vec3 Normal { 0.0f, 1.0f, 0.0f };
			glm::vec2 TexCoord { 0.0f };
			SymmetricMatrix Q;
			std::vector<uint32_t> Faces;
			bool Valid = true;
			bool IsBorder = false;
		};

		struct EdgeKey
		{
			uint32_t u = 0;
			uint32_t v = 0;
			bool operator==(const EdgeKey& o) const { return u == o.u && v == o.v; }
		};

		struct EdgeKeyHash
		{
			size_t operator()(const EdgeKey& k) const
			{
				return (static_cast<size_t>(k.u) << 32) ^ static_cast<size_t>(k.v);
			}
		};

		struct EdgeCandidate
		{
			uint32_t u = 0;
			uint32_t v = 0;
			float Cost = 0.0f;
			glm::vec3 TargetPos { 0.0f };
			uint64_t Timestamp = 0;

			bool operator>(const EdgeCandidate& o) const
			{
				return Cost > o.Cost;
			}
		};
	}

	bool MeshSimplifier::Simplify(
		const WModelVertex* vertices, size_t vertexCount,
		const uint32_t* indices, size_t indexCount,
		const SimplificationOptions& options,
		SimplificationResult& outResult,
		std::string* outError)
	{
		if (!vertices || vertexCount == 0 || !indices || indexCount < 3)
		{
			outResult = SimplificationResult{};
			if (outError) *outError = "Invalid input mesh: null or empty buffers";
			return false;
		}

		uint32_t rawTriCount = static_cast<uint32_t>(indexCount / 3);

		// 如果目标保留比例为 1.0 或更高，直接原样拷贝输出
		if (options.TargetRatio >= 1.0f)
		{
			outResult.Vertices.assign(vertices, vertices + vertexCount);
			outResult.Indices.assign(indices, indices + indexCount);
			outResult.OriginalTriangles = rawTriCount;
			outResult.SimplifiedTriangles = rawTriCount;
			outResult.ActualRatio = 1.0f;
			outResult.MaxErrorObserved = 0.0f;
			return true;
		}

		std::vector<SimpVertex> verts(vertexCount);
		for (size_t i = 0; i < vertexCount; ++i)
		{
			verts[i].Position = vertices[i].Position;
			verts[i].Normal = vertices[i].Normal;
			verts[i].TexCoord = vertices[i].TexCoord;
		}

		std::vector<SimpTriangle> triangles;
		triangles.reserve(rawTriCount);

		// 1. 初始化面与顶点二次型
		for (uint32_t i = 0; i < rawTriCount; ++i)
		{
			uint32_t i0 = indices[i * 3 + 0];
			uint32_t i1 = indices[i * 3 + 1];
			uint32_t i2 = indices[i * 3 + 2];

			if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
				continue;
			if (i0 == i1 || i1 == i2 || i0 == i2)
				continue;

			glm::vec3 p0 = verts[i0].Position;
			glm::vec3 p1 = verts[i1].Position;
			glm::vec3 p2 = verts[i2].Position;

			glm::vec3 fn = glm::cross(p1 - p0, p2 - p0);
			float len = glm::length(fn);
			if (len < 1e-12f)
				continue; // 忽略退化面

			fn /= len;
			double d = -glm::dot(fn, p0);
			SymmetricMatrix Kp(fn.x, fn.y, fn.z, d);

			verts[i0].Q += Kp;
			verts[i1].Q += Kp;
			verts[i2].Q += Kp;

			uint32_t triIdx = static_cast<uint32_t>(triangles.size());
			SimpTriangle tri;
			tri.v[0] = i0;
			tri.v[1] = i1;
			tri.v[2] = i2;
			tri.Normal = fn;
			triangles.push_back(tri);

			verts[i0].Faces.push_back(triIdx);
			verts[i1].Faces.push_back(triIdx);
			verts[i2].Faces.push_back(triIdx);
		}

		uint32_t originalTriangles = static_cast<uint32_t>(triangles.size());
		if (originalTriangles == 0)
		{
			outResult.OriginalTriangles = rawTriCount;
			outResult.SimplifiedTriangles = 0;
			outResult.ActualRatio = 0.0f;
			outResult.MaxErrorObserved = 0.0f;
			return true;
		}

		// 2. 统计边与边界检测
		std::unordered_map<EdgeKey, std::vector<uint32_t>, EdgeKeyHash> edgeFaces;
		edgeFaces.reserve(triangles.size() * 3);

		for (uint32_t t = 0; t < triangles.size(); ++t)
		{
			const auto& tri = triangles[t];
			for (int e = 0; e < 3; ++e)
			{
				uint32_t a = tri.v[e];
				uint32_t b = tri.v[(e + 1) % 3];
				uint32_t u = std::min(a, b);
				uint32_t v = std::max(a, b);
				edgeFaces[{u, v}].push_back(t);
			}
		}

		std::unordered_set<EdgeKey, EdgeKeyHash> boundaryEdges;
		for (const auto& [edge, fList] : edgeFaces)
		{
			if (fList.size() == 1)
			{
				boundaryEdges.insert(edge);
				verts[edge.u].IsBorder = true;
				verts[edge.v].IsBorder = true;

				if (options.PreserveBorders)
				{
					uint32_t triIdx = fList[0];
					glm::vec3 fn = triangles[triIdx].Normal;
					glm::vec3 pU = verts[edge.u].Position;
					glm::vec3 pV = verts[edge.v].Position;
					glm::vec3 edgeDir = pV - pU;
					float eLen = glm::length(edgeDir);
					if (eLen > 1e-12f)
					{
						edgeDir /= eLen;
						glm::vec3 bn = glm::cross(edgeDir, fn);
						float bnLen = glm::length(bn);
						if (bnLen > 1e-12f)
						{
							bn /= bnLen;
							double bd = -glm::dot(bn, pU);
							SymmetricMatrix bKp(bn.x, bn.y, bn.z, bd);
							for (int k = 0; k < 10; ++k)
								bKp.m[k] *= 1000.0;
							verts[edge.u].Q += bKp;
							verts[edge.v].Q += bKp;
						}
					}
				}
			}
		}

		// 3. 计算边折叠代价辅助函数
		auto EvaluateEdgeCost = [&](uint32_t u, uint32_t v, glm::vec3& outTargetPos, float& outCost) -> bool
		{
			SymmetricMatrix Q = verts[u].Q + verts[v].Q;
			glm::vec3 pU = verts[u].Position;
			glm::vec3 pV = verts[v].Position;
			glm::vec3 pMid = 0.5f * (pU + pV);

			bool isBoundaryEdge = boundaryEdges.find({std::min(u, v), std::max(u, v)}) != boundaryEdges.end();

			if (options.PreserveBorders)
			{
				if (verts[u].IsBorder && verts[v].IsBorder && !isBoundaryEdge)
				{
					return false; // 内部割线，不可折叠
				}

				if (verts[u].IsBorder && !verts[v].IsBorder)
				{
					outTargetPos = pU;
					outCost = static_cast<float>(std::max(0.0, Q.Evaluate(pU))) + 100.0f;
					return true;
				}
				if (!verts[u].IsBorder && verts[v].IsBorder)
				{
					outTargetPos = pV;
					outCost = static_cast<float>(std::max(0.0, Q.Evaluate(pV))) + 100.0f;
					return true;
				}

				if (isBoundaryEdge)
				{
					double eU = Q.Evaluate(pU);
					double eV = Q.Evaluate(pV);
					double eM = Q.Evaluate(pMid);
					double minE = eU;
					outTargetPos = pU;
					if (eV < minE) { minE = eV; outTargetPos = pV; }
					if (eM < minE) { minE = eM; outTargetPos = pMid; }
					outCost = static_cast<float>(std::max(0.0, minE)) + 50.0f;
					return true;
				}
			}

			double eU = Q.Evaluate(pU);
			double eV = Q.Evaluate(pV);
			double eM = Q.Evaluate(pMid);

			double minE = eU;
			outTargetPos = pU;
			if (eV < minE) { minE = eV; outTargetPos = pV; }
			if (eM < minE) { minE = eM; outTargetPos = pMid; }

			outCost = static_cast<float>(std::max(0.0, minE));
			return true;
		};

		// 4. 法线翻转检验辅助函数
		auto CheckNormalFlipping = [&](uint32_t u, uint32_t v, const glm::vec3& targetPos) -> bool
		{
			for (uint32_t fIdx : verts[u].Faces)
			{
				const auto& tri = triangles[fIdx];
				if (tri.Deleted) continue;
				if (tri.v[0] == v || tri.v[1] == v || tri.v[2] == v)
					continue;

				glm::vec3 p[3];
				for (int k = 0; k < 3; ++k)
					p[k] = (tri.v[k] == u) ? targetPos : verts[tri.v[k]].Position;

				glm::vec3 newN = glm::cross(p[1] - p[0], p[2] - p[0]);
				if (glm::dot(newN, tri.Normal) <= 0.0f)
					return true;
			}

			for (uint32_t fIdx : verts[v].Faces)
			{
				const auto& tri = triangles[fIdx];
				if (tri.Deleted) continue;
				if (tri.v[0] == u || tri.v[1] == u || tri.v[2] == u)
					continue;

				glm::vec3 p[3];
				for (int k = 0; k < 3; ++k)
					p[k] = (tri.v[k] == v) ? targetPos : verts[tri.v[k]].Position;

				glm::vec3 newN = glm::cross(p[1] - p[0], p[2] - p[0]);
				if (glm::dot(newN, tri.Normal) <= 0.0f)
					return true;
			}

			return false;
		};

		// 5. 建立最小堆优先队列
		std::priority_queue<EdgeCandidate, std::vector<EdgeCandidate>, std::greater<EdgeCandidate>> pq;
		std::unordered_map<EdgeKey, uint64_t, EdgeKeyHash> edgeTimestamps;
		uint64_t nextTimestamp = 0;

		for (const auto& [edge, fList] : edgeFaces)
		{
			glm::vec3 targetPos;
			float cost = 0.0f;
			if (EvaluateEdgeCost(edge.u, edge.v, targetPos, cost))
			{
				uint64_t ts = ++nextTimestamp;
				edgeTimestamps[edge] = ts;
				pq.push({edge.u, edge.v, cost, targetPos, ts});
			}
		}

		uint32_t targetTriangles = static_cast<uint32_t>(std::ceil(options.TargetRatio * static_cast<float>(rawTriCount)));
		if (targetTriangles < 1) targetTriangles = 1;
		if (targetTriangles > originalTriangles) targetTriangles = originalTriangles;
		uint32_t activeTriangles = originalTriangles;
		float maxErrorObserved = 0.0f;

		// 6. 迭代边折叠
		while (!pq.empty() && activeTriangles > targetTriangles)
		{
			EdgeCandidate cand = pq.top();
			pq.pop();

			uint32_t u = cand.u;
			uint32_t v = cand.v;

			if (!verts[u].Valid || !verts[v].Valid)
				continue;

			EdgeKey ek{std::min(u, v), std::max(u, v)};
			auto itTs = edgeTimestamps.find(ek);
			if (itTs == edgeTimestamps.end() || itTs->second != cand.Timestamp)
				continue;

			if (cand.Cost > options.MaxError)
				break;

			if (options.CheckNormalFlip && CheckNormalFlipping(u, v, cand.TargetPos))
				continue;

			maxErrorObserved = std::max(maxErrorObserved, cand.Cost);

			// 插值更新顶点属性并合并 v 到 u
			glm::vec3 diff = verts[v].Position - verts[u].Position;
			float d2 = glm::dot(diff, diff);
			float t = 0.5f;
			if (d2 > 1e-12f)
			{
				t = glm::clamp(glm::dot(cand.TargetPos - verts[u].Position, diff) / d2, 0.0f, 1.0f);
			}
			verts[u].Position = cand.TargetPos;
			glm::vec3 n = glm::mix(verts[u].Normal, verts[v].Normal, t);
			float nLen = glm::length(n);
			verts[u].Normal = (nLen > 1e-6f) ? (n / nLen) : verts[u].Normal;
			verts[u].TexCoord = glm::mix(verts[u].TexCoord, verts[v].TexCoord, t);
			verts[u].Q += verts[v].Q;
			verts[u].IsBorder = (verts[u].IsBorder || verts[v].IsBorder);
			verts[v].Valid = false;

			std::vector<uint32_t> affectedFaces;
			affectedFaces.reserve(verts[u].Faces.size() + verts[v].Faces.size());

			for (uint32_t fIdx : verts[v].Faces)
			{
				auto& tri = triangles[fIdx];
				if (tri.Deleted) continue;

				if (tri.v[0] == u || tri.v[1] == u || tri.v[2] == u)
				{
					tri.Deleted = true;
					if (activeTriangles > 0) --activeTriangles;
				}
				else
				{
					for (int k = 0; k < 3; ++k)
					{
						if (tri.v[k] == v) tri.v[k] = u;
					}
					affectedFaces.push_back(fIdx);
				}
			}

			for (uint32_t fIdx : verts[u].Faces)
			{
				auto& tri = triangles[fIdx];
				if (tri.Deleted) continue;

				if (tri.v[0] == tri.v[1] || tri.v[1] == tri.v[2] || tri.v[0] == tri.v[2])
				{
					tri.Deleted = true;
					if (activeTriangles > 0) --activeTriangles;
				}
				else
				{
					affectedFaces.push_back(fIdx);
				}
			}

			verts[u].Faces.clear();
			std::unordered_set<uint32_t> neighborVerts;
			for (uint32_t fIdx : affectedFaces)
			{
				auto& tri = triangles[fIdx];
				if (tri.Deleted) continue;

				glm::vec3 fn = glm::cross(verts[tri.v[1]].Position - verts[tri.v[0]].Position,
				                          verts[tri.v[2]].Position - verts[tri.v[0]].Position);
				float fnLen = glm::length(fn);
				if (fnLen > 1e-12f)
				{
					tri.Normal = fn / fnLen;
					verts[u].Faces.push_back(fIdx);

					for (int k = 0; k < 3; ++k)
					{
						if (tri.v[k] != u && verts[tri.v[k]].Valid)
							neighborVerts.insert(tri.v[k]);
					}
				}
				else
				{
					tri.Deleted = true;
					if (activeTriangles > 0) --activeTriangles;
				}
			}

			for (uint32_t w : neighborVerts)
			{
				EdgeKey newEk{std::min(u, w), std::max(u, w)};
				glm::vec3 newTarget;
				float newCost = 0.0f;
				if (EvaluateEdgeCost(newEk.u, newEk.v, newTarget, newCost))
				{
					uint64_t ts = ++nextTimestamp;
					edgeTimestamps[newEk] = ts;
					pq.push({newEk.u, newEk.v, newCost, newTarget, ts});
				}
			}
		}

		// 7. 紧凑输出网格缓冲
		std::vector<int32_t> remap(verts.size(), -1);
		outResult.Vertices.clear();
		outResult.Indices.clear();

		for (const auto& tri : triangles)
		{
			if (tri.Deleted) continue;
			for (int k = 0; k < 3; ++k)
			{
				uint32_t vid = tri.v[k];
				if (remap[vid] == -1)
				{
					remap[vid] = static_cast<int32_t>(outResult.Vertices.size());
					WModelVertex mv;
					mv.Position = verts[vid].Position;
					mv.Normal = verts[vid].Normal;
					mv.TexCoord = verts[vid].TexCoord;
					outResult.Vertices.push_back(mv);
				}
				outResult.Indices.push_back(static_cast<uint32_t>(remap[vid]));
			}
		}

		outResult.OriginalTriangles = rawTriCount;
		outResult.SimplifiedTriangles = static_cast<uint32_t>(outResult.Indices.size() / 3);
		outResult.ActualRatio = rawTriCount > 0 ? (static_cast<float>(outResult.SimplifiedTriangles) / static_cast<float>(rawTriCount)) : 0.0f;
		outResult.MaxErrorObserved = maxErrorObserved;

		return true;
	}
}

