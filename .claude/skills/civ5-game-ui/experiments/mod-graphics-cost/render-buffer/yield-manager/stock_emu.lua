-- Stock (Firaxis) YieldIconManager logic, rebuilt in the live YieldIconManager state from VP's instances.
-- Driven by the engine's Events.ShowHexYield; parented to the context, not to VP's Controls.Anchors.
local S = vp.S or { vis = {}, apool = {}, ipool = {}, spool = {}, on = false, n = 0, sprites = 0, events = 0 }
vp.S = S
local function img(parent, ox, oy, tex)
  local t = table.remove(S.ipool)
  if not t then t = {} ContextPtr:BuildInstanceForControl("ImageInstance", t, parent) else t.Image:ChangeParent(parent) end
  t.Image:SetTexture(tex) t.Image:SetTextureOffsetVal(ox, oy) t.Image:SetHide(false)
  S.sprites = S.sprites + 1
  return t
end
local function destroy(i)
  local r = S.vis[i] if not r then return end
  for _, t in ipairs(r.imgs) do t.Image:SetHide(true) S.ipool[#S.ipool + 1] = t S.sprites = S.sprites - 1 end
  r.stk.Stack:SetHide(true) S.spool[#S.spool + 1] = r.stk
  r.anc.Anchor:SetHide(true) S.apool[#S.apool + 1] = r.anc
  S.vis[i] = nil S.n = S.n - 1
end
local function numoff(n) if n > 12 then n = 12 end return 128 * ((n - 6) % 4), (n > 9) and 768 or 640 end
local function build(x, y, i, plot)
  if not plot:IsRevealed(Game.GetActiveTeam(), false) then return end
  local a = { plot:CalculateYield(0, true), plot:CalculateYield(1, true), plot:CalculateYield(2, true), plot:CalculateYield(3, true) }
  local cul = plot:CalculateYield(4, true) -- stock calls plot:GetCulture(), which VP's DLL no longer has
  if a[1] == 0 and a[2] == 0 and a[3] == 0 and a[4] == 0 and cul == 0 then return end
  local anc = table.remove(S.apool)
  if not anc then anc = {} ContextPtr:BuildInstanceForControl("AnchorInstance", anc, ContextPtr) end
  local stk = table.remove(S.spool)
  if not stk then stk = {} ContextPtr:BuildInstanceForControl("StackInstance", stk, anc.Stacks) else stk.Stack:ChangeParent(anc.Stacks) end
  local r = { anc = anc, stk = stk, imgs = {} }
  for k = 1, 4 do
    local n = a[k]
    if n > 0 then
      local t = img(stk.Stack, (k - 1) * 128, (n >= 6) and 512 or 128 * (n - 1), "YieldAtlas.dds")
      r.imgs[#r.imgs + 1] = t
      if n > 5 then local ox, oy = numoff(n) r.imgs[#r.imgs + 1] = img(t.Image, ox, oy, "YieldAtlas.dds") end
    end
  end
  if cul > 0 then r.imgs[#r.imgs + 1] = img(stk.Stack, 0, (cul >= 5) and 512 or 128 * (cul - 1), "YieldAtlas_128_Culture.dds") end
  stk.Stack:SetHide(false) stk.Stack:CalculateSize() stk.Stack:ReprocessAnchoring()
  anc.Stacks:CalculateSize() anc.Stacks:ReprocessAnchoring()
  anc.Anchor:SetHexPosition(x, y) anc.Anchor:SetHide(false)
  S.vis[i] = r S.n = S.n + 1
end
S.h = function(x, y, show)
  S.events = S.events + 1
  local plot = Map.GetPlot(x, y) if not plot then return end
  local i = plot:GetPlotIndex()
  S.shown = S.shown or {}
  S.shown[i] = show or nil
  if not S.on then return end
  destroy(i)
  if show then build(x, y, i, plot) end
end
S.clear = function() local k = {} for i in pairs(S.vis) do k[#k + 1] = i end for _, i in ipairs(k) do destroy(i) end end
if not S.added then S.added = true Events.ShowHexYield.Add(function(x, y, b) if vp.S and vp.S.h then vp.S.h(x, y, b) end end) end
return "installed", S.n, S.sprites
