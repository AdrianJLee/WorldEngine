#include "World/Asset/AssetThumbnailCache.h"
#include "World/Core/Log.h"

#include <cassert>
#include <iostream>

using namespace World;

static void TestCategoryDetection()
{
	assert(AssetThumbnailCache::DetectCategory("assets/textures/hero.png") == AssetCategory::Texture);
	assert(AssetThumbnailCache::DetectCategory("assets/textures/sky.hdr") == AssetCategory::Texture);
	assert(AssetThumbnailCache::DetectCategory("assets/materials/brick.wmat") == AssetCategory::Material);
	assert(AssetThumbnailCache::DetectCategory("assets/models/character.glb") == AssetCategory::Mesh);
	assert(AssetThumbnailCache::DetectCategory("assets/models/tree.obj") == AssetCategory::Mesh);
	assert(AssetThumbnailCache::DetectCategory("assets/prefabs/coin.prefab") == AssetCategory::Prefab);
	assert(AssetThumbnailCache::DetectCategory("assets/scripts/player.luau") == AssetCategory::Script);
	assert(AssetThumbnailCache::DetectCategory("assets/audio/explosion.wav") == AssetCategory::Audio);
	assert(AssetThumbnailCache::DetectCategory("assets/scenes/level01.wscene") == AssetCategory::Level);
	assert(AssetThumbnailCache::DetectCategory("assets/misc/readme.txt") == AssetCategory::Unknown);
	assert(AssetThumbnailCache::DetectCategory("assets/vfx/flame_tornado.wvfx") == AssetCategory::VFX);
	assert(AssetThumbnailCache::DetectCategory("assets/vfx/sparks.wparticle") == AssetCategory::VFX);

	std::cout << "[PASS] TestCategoryDetection\n";
}

static void TestThumbnailRegistrationAndCaching()
{
	auto& cache = AssetThumbnailCache::Get();
	cache.Clear();
	assert(cache.Count() == 0);

	const std::string texPath = "assets/textures/ground.png";
	assert(!cache.HasThumbnail(texPath));

	// GetOrCreateThumbnail should instantiate an entry with detected category
	ThumbnailInfo& created = cache.GetOrCreateThumbnail(texPath);
	assert(created.Path == texPath);
	assert(created.Category == AssetCategory::Texture);
	assert(created.Width == 64);
	assert(created.Height == 64);
	assert(!created.IsReady);
	assert(cache.Count() == 1);
	assert(cache.HasThumbnail(texPath));

	// Modify entry in cache
	created.TextureId = 1001;
	created.IsReady = true;

	const ThumbnailInfo* fetched = cache.GetThumbnail(texPath);
	assert(fetched != nullptr);
	assert(fetched->TextureId == 1001);
	assert(fetched->IsReady);

	// Register another thumbnail manually
	ThumbnailInfo matThumb;
	matThumb.Path = "assets/materials/gold.wmat";
	matThumb.TextureId = 2002;
	matThumb.IsReady = true;
	cache.RegisterThumbnail(matThumb.Path, matThumb);

	assert(cache.Count() == 2);
	const ThumbnailInfo* fetchedMat = cache.GetThumbnail(matThumb.Path);
	assert(fetchedMat != nullptr);
	assert(fetchedMat->Category == AssetCategory::Material);
	assert(fetchedMat->TextureId == 2002);

	cache.Clear();
	assert(cache.Count() == 0);
	assert(!cache.HasThumbnail(texPath));

	std::cout << "[PASS] TestThumbnailRegistrationAndCaching\n";
}

int main()
{
	Log::Init();
	std::cout << "Running WorldAssetPickerTests...\n";

	TestCategoryDetection();
	TestThumbnailRegistrationAndCaching();

	std::cout << "All WorldAssetPickerTests passed successfully.\n";
	return 0;
}
