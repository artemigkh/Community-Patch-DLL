// shaders.hlsl - all shaders of the prototype, compiled offline by build.cmd
// with fxc into build\gen\*.h (shader model 4.0 so a feature-level-10 device
// can run them too).

// ---------------------------------------------------------------------------
// Fullscreen triangle (no vertex buffer, no input layout): 3 vertices from
// SV_VertexID cover the whole viewport.
struct FsOut
{
	float4 pos : SV_Position;
	float2 uv  : TEXCOORD0;
};

FsOut VSFullscreen(uint id : SV_VertexID)
{
	FsOut o;
	float2 uv = float2((id << 1) & 2, id & 2);
	o.uv = uv;
	o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	return o;
}

// ---------------------------------------------------------------------------
// shell_composite.dll: sample the local copy of the shared texture.
Texture2D    g_src : register(t0);
SamplerState g_smp : register(s0);

float4 PSComposite(FsOut i) : SV_Target
{
	return g_src.Sample(g_smp, i.uv);
}

// ---------------------------------------------------------------------------
// shell_server.exe: the "world": hex grid + spinning quad + frame barcode.
cbuffer ServerCB : register(b0)
{
	float  g_time;
	float  g_pad0;
	float2 g_size;
	uint   g_frameLo;
	uint   g_frameHi;
	float2 g_pad1;
};

float hash21(float2 p)
{
	p = frac(p * float2(123.34, 456.21));
	p += dot(p, p + 45.32);
	return frac(p.x * p.y);
}

float4 PSServer(FsOut i) : SV_Target
{
	float2 px = i.pos.xy;

	// hex grid (pointy-top), cell size ~ 1/24 of the height
	float scale = g_size.y / 24.0;
	float2 p = px / scale;
	const float2 s = float2(1.0, 1.7320508);
	float4 hc = floor(float4(p, p - float2(0.5, 1.0)) / s.xyxy) + 0.5;
	float4 h = float4(p - hc.xy * s, p - (hc.zw + 0.5) * s);
	bool first = dot(h.xy, h.xy) < dot(h.zw, h.zw);
	float2 local = first ? h.xy : h.zw;
	float2 id = first ? hc.xy : hc.zw + 0.5;
	float2 a = abs(local);
	float d = max(dot(a, s * 0.5), a.x);          // 0 at centre, 0.5 at the edge
	float edge = smoothstep(0.44, 0.5, d);
	float r = hash21(id);
	float3 land = lerp(float3(0.10, 0.35, 0.12), float3(0.55, 0.50, 0.25), r);
	float3 water = float3(0.05, 0.20, 0.45 + 0.1 * sin(g_time * 2.0 + r * 6.28));
	float3 col = (r < 0.35) ? water : land;
	col = lerp(col, float3(0.85, 0.85, 0.75), edge * 0.8);

	// spinning quad in the middle
	float2 c = g_size * 0.5;
	float ang = g_time * 1.5;
	float2 q = px - c;
	float2 rq = float2(q.x * cos(ang) + q.y * sin(ang), -q.x * sin(ang) + q.y * cos(ang));
	float hsz = min(g_size.x, g_size.y) * 0.18;
	if (abs(rq.x) < hsz && abs(rq.y) < hsz)
	{
		float2 uv = rq / (2.0 * hsz) + 0.5;
		col = float3(uv.x, uv.y, 1.0 - uv.x * uv.y);
	}

	// moving vertical bar: x = frame counter mod width (shows tearing / pacing)
	float barX = (float)(g_frameLo % (uint)g_size.x);
	if (abs(px.x - barX) < 3.0)
		col = float3(1.0, 0.2, 0.9);

	// frame-counter barcode: 32 cells of 12x12 px along the top-left edge,
	// bit k of the frame counter -> white (1) / black (0); red frame around it.
	if (px.y >= 4.0 && px.y < 16.0 && px.x >= 4.0 && px.x < 4.0 + 32.0 * 12.0)
	{
		uint k = (uint)((px.x - 4.0) / 12.0);
		uint bit = (g_frameLo >> k) & 1u;
		col = bit ? float3(1, 1, 1) : float3(0, 0, 0);
	}
	else if (px.y >= 2.0 && px.y < 18.0 && px.x >= 2.0 && px.x < 6.0 + 32.0 * 12.0)
	{
		col = float3(1, 0, 0);
	}
	return float4(col, 1.0);
}

// ---------------------------------------------------------------------------
// test_host.exe: "UI" rectangles. Each instance is one rectangle from the
// constant buffer: rect in pixels, colour (alpha = opacity) and a style flag
// (0 = flat, 1 = text-like noise: glyph-shaped alpha holes).
struct UiRect
{
	float4 rect;   // x0, y0, x1, y1 in pixels
	float4 color;  // rgba, straight alpha
	float4 style;  // x = style, y = seed
};

cbuffer HostCB : register(b0)
{
	float2 g_vp;       // viewport size in pixels
	float  g_htime;
	float  g_hpad;
	UiRect g_rects[64];
};

struct UiOut
{
	float4 pos   : SV_Position;
	float2 local : TEXCOORD0;   // pixels relative to the rect origin
	nointerpolation uint inst : TEXCOORD1;
};

UiOut VSHostUi(uint vid : SV_VertexID, uint inst : SV_InstanceID)
{
	UiRect r = g_rects[inst];
	// two triangles: 0 1 2, 2 1 3
	uint corner[6] = { 0, 1, 2, 2, 1, 3 };
	uint cv = corner[vid];
	float2 t = float2(cv & 1, cv >> 1);
	float2 px = lerp(r.rect.xy, r.rect.zw, t);
	UiOut o;
	o.pos = float4(px / g_vp * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	o.local = px - r.rect.xy;
	o.inst = inst;
	return o;
}

float4 PSHostUi(UiOut i) : SV_Target
{
	UiRect r = g_rects[i.inst];
	float4 col = r.color;
	if (r.style.x > 0.5)
	{
		// rows of 8x12 "glyph" cells with random 3x5 pixel-block patterns
		float2 cell = floor(i.local / float2(8.0, 14.0));
		float2 inCell = i.local - cell * float2(8.0, 14.0);
		float lineLen = 10.0 + 30.0 * hash21(float2(cell.y, r.style.y));
		if (cell.x >= lineLen || inCell.x >= 6.0 || inCell.y >= 10.0)
			discard;
		float2 blk = floor(inCell / 2.0);
		float on = step(0.45, hash21(cell * 7.0 + blk + r.style.y + floor(g_htime * 0.5)));
		if (on < 0.5)
			discard;
	}
	return col;
}
