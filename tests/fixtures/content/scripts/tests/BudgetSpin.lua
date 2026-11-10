-- P2 W6 headless regression fixture.
-- The test host applies a sandbox budget and expects this script's OnUpdate to be
-- interrupted by it (the instance faults, other scripts in the scene keep running).
-- The spin lives in OnUpdate on purpose: a top-level loop would fault at load time.
---@class BudgetSpin : WorldScript
---@field Spins integer
local BudgetSpin = { Spins = 0 }

function BudgetSpin:OnCreate()
    self.Spins = 0
end

---@param dt number
function BudgetSpin:OnUpdate(dt)
    assert(type(dt) == "number" and dt >= 0)
    -- Intentional: only the sandbox budget can stop this loop.
    while true do
        self.Spins = self.Spins + 1
    end
end

return BudgetSpin
