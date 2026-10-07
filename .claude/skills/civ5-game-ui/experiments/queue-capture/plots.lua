local t = {}
local n = Map.GetNumPlots()
for i = 0, n - 1 do
  local p = Map.GetPlotByIndex(i)
  local c = p:GetPlotCity()
  t[#t + 1] = table.concat({p:GetX(), p:GetY(), p:GetImprovementType(), p:GetRouteType(), p:GetFeatureType(), p:GetResourceType(-1), p:GetOwner(), p:IsCity() and 1 or 0, p:GetTerrainType(), p:GetPlotType(), p:IsImprovementPillaged() and 1 or 0, p:GetNumUnits(), p:IsRiver() and 1 or 0, (p:GetWorkingCity() and p:GetWorkingCity():GetID() or -1)}, ",")
end
return table.concat(t, ";")
