// srv_shadow_tap.cpp - server.exe's reader of the model shadow map tap (epic 0169, issue 4304, ticket 0312; C1 accepted,
// MC #1804). Contract: srv_shadow_tap.h. Written 2026-11-15 for this repository (GPL-3.0-or-later). Model:
// srv_overlay_tap.cpp (epic 0167). Producer: shell/composite/shadow_tap.cpp. Layout: shell/common/shell_shadow_tap.h.
// Design: research/05-graphics-composition/terrain-surface.md sections 11 and 11.9.
//
// The slot hand-over (Dekker style, both sides with full barriers), as the overlay's:
//   reader:   inUseSlot = s; barrier; read slot s (seq even, valid, generation = the block's) -> bind, ack; else go back
//   producer: slot s seq odd, valid = 0; barrier; read inUseSlot; if it is s -> restore s and do not write it
#include "srv_shadow_tap.h"
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <math.h>
#include <stdarg.h>
#include <vector>
#include "vp_shared_tex.h"

namespace srvrender {

namespace {

const char* kStateNames[] = {"NONE", "ACTIVE", "FAILED", "OFF"};
const char* kFailNames[] = {"NONE", "PATTERN", "HANDLE", "DESC", "SHARED", "EXCEPTION", "VIEWS", "KILLED", "VERSION", "QUALITY"};
const uint32_t kR16 = 56, kRgba16f = 10;

const char* FailName(uint32_t r)
{
	return r < sizeof(kFailNames) / sizeof(kFailNames[0]) ? kFailNames[r] : "?";
}

std::string Fmt(const char* fmt, ...)
{
	char b[600];
	va_list a;
	va_start(a, fmt);
	_vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, a);
	va_end(a);
	return b;
}

uint32_t BytesPerTexel(uint32_t f)
{
	return f == kRgba16f ? 8u : f == kR16 ? 2u : 0u;
}

// A consistent copy of a slot (per-slot seqlock); false while the producer writes it.
bool SnapSlot(const VPShellShadowSlot* src, VPShellShadowSlot& out)
{
	for(int i = 0; i < 16; ++i)
	{
		const uint32_t s1 = src->seq;
		if(s1 & 1)
		{
			YieldProcessor();
			continue;
		}
		MemoryBarrier();
		memcpy(&out, (const void*)src, sizeof(out));
		MemoryBarrier();
		if(src->seq == s1)
		{
			out.seq = s1;
			return true;
		}
	}
	return false;
}

// The description the EXE's shadow setup makes for quality q (terrain-surface.md 11, S2b); "" when the slot has it
std::string DescProblem(const VPShellShadowSlot& s)
{
	if(s.quality == 0)
	{
		if(!(s.flags & VPSHD_SLOT_NO_TEXTURE) || s.strength != 0.0f)
			return Fmt("quality 0 needs a slot without texture and strength 0 (flags 0x%X, strength %g)", s.flags, s.strength);
		return "";
	}
	if(s.flags & VPSHD_SLOT_NO_TEXTURE)
		return Fmt("quality %u slot without a texture", s.quality);
	bool ok;
	if(s.quality == 3)
		ok = s.width == 2048 && s.height == 2048 && s.dxgiFormat == kR16;
	else if(s.quality == 2)
		ok = s.width == 1024 && s.height == 1024 && s.dxgiFormat == kR16;
	else
		ok = s.dxgiFormat == kRgba16f && s.width >= 1 && s.height >= 1 && s.width <= 16384 && s.height <= 16384;
	if(!ok || s.mips != 1)
		return Fmt("quality %u slot describes %u x %u, %u mips, format %u (q 3: 2048 x 2048 R16_UNORM; q 2: 1024 x 1024 "
		           "R16_UNORM; q 1: R16G16B16A16_FLOAT; 1 mip)",
		           s.quality, s.width, s.height, s.mips, s.dxgiFormat);
	return "";
}

}  // namespace

struct ShadowTapReader::Impl
{
	ID3D11Device1* dev1 = nullptr;
	HANDLE secH = nullptr;
	uint8_t* sec = nullptr;
	struct Tex
	{
		ID3D11Texture2D* tex = nullptr;
		ID3D11ShaderResourceView* srv = nullptr;
		uint32_t epoch = 0;
		uint32_t pid = 0;
	} tex[VPSHELL_SHADOW_SLOTS];
	ShadowFrame bound;             // srv not referenced here: tex[bound.slot].srv owns it (null at quality 0)
	bool haveBound = false;
	uint32_t lastBlockState = 0xFFFFFFFFu;
	uint32_t lastBlockFail = 0xFFFFFFFFu;
	uint32_t lastGen = 0;
	int64_t lastGenPresent = 0;
	uint64_t lastGenMapFrames = 0;
	uint64_t lastGenMs = 0;
	bool staleEpisode = false;
	bool noMapGap = false;
	uint32_t refusedLogs = 0;
	std::string lastRefusal;
	ID3D11Texture2D* staging = nullptr;   // verify read-back
	D3D11_TEXTURE2D_DESC stagingDesc = {};

	VPShellShadowTap* blk() const { return (VPShellShadowTap*)sec; }
	void closeSection()
	{
		if(sec)
			UnmapViewOfFile(sec);
		if(secH)
			CloseHandle(secH);
		sec = nullptr;
		secH = nullptr;
	}
	void dropTex(uint32_t i)
	{
		SafeRelease(tex[i].srv);
		SafeRelease(tex[i].tex);
		tex[i].epoch = 0;
		tex[i].pid = 0;
	}
	~Impl()
	{
		for(uint32_t i = 0; i < VPSHELL_SHADOW_SLOTS; ++i)
			dropTex(i);
		SafeRelease(staging);
		if(sec && blk()->inUseSlot != VPSHELL_SHADOW_NO_SLOT)
			InterlockedExchange((volatile LONG*)&blk()->inUseSlot, (LONG)VPSHELL_SHADOW_NO_SLOT);
		closeSection();
		SafeRelease(dev1);
	}
};

ShadowTapReader::ShadowTapReader(uint32_t exePid, VPShellHeader* hdr, ID3D11Device* dev) : m_pid(exePid), m_hdr(hdr), m_dev(dev)
{
	wchar_t name[64];
	_snwprintf_s(name, _countof(name), _TRUNCATE, VPSHELL_SHADOWTAP_SECTION_FMT, exePid);
	m_name = name;
}

ShadowTapReader::ShadowTapReader(const std::wstring& sectionName, uint32_t exePid, VPShellHeader* hdr, ID3D11Device* dev)
    : m_name(sectionName), m_pid(exePid), m_hdr(hdr), m_dev(dev)
{
}

ShadowTapReader::~ShadowTapReader()
{
	release();
}

void ShadowTapReader::release()
{
	delete m_impl;   // names no slot in inUseSlot, drops the textures and the mapping
	m_impl = nullptr;
}

bool ShadowTapReader::acquire(ShadowFrame& out)
{
	out = ShadowFrame{};
	if(!m_dev || !m_pid)
	{
		m_state = State::Absent;
		return false;
	}
	if(m_state == State::Failed)
		return false;   // final; lastWhy() says why
	if(!m_impl)
		m_impl = new Impl();
	Impl& im = *m_impl;
	if(!im.sec)
	{
		im.secH = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, m_name.c_str());
		im.sec = im.secH ? (uint8_t*)MapViewOfFile(im.secH, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0) : nullptr;
		if(!im.sec)
		{
			im.closeSection();
			m_state = State::Absent;   // no producer (yet), or an EXE without the tap
			return false;
		}
		MEMORY_BASIC_INFORMATION mbi = {};
		const size_t bytes = VirtualQuery(im.sec, &mbi, sizeof(mbi)) ? mbi.RegionSize : 0;
		if(bytes < VPSHELL_SHADOWTAP_SECTION_BYTES)
		{
			im.closeSection();
			m_failReason = VPSHDF_VERSION;
			m_lastWhy = Fmt("the shadow tap's section '%ls' is %llu bytes, the layout needs %u (refused by the server)",
			                m_name.c_str(), (unsigned long long)bytes, VPSHELL_SHADOWTAP_SECTION_BYTES);
			m_state = State::Failed;
			RNote("shadow: LOUD: %s (VPSHDF_VERSION)", m_lastWhy.c_str());
			release();
			return false;
		}
	}
	VPShellShadowTap* live = im.blk();
	VPShellShadowTap tb;
	auto keepBound = [&]() -> bool {
		if(im.haveBound && m_state == State::Active)
		{
			out = im.bound;
			out.srv = im.bound.slot < VPSHELL_SHADOW_SLOTS ? im.tex[im.bound.slot].srv : nullptr;
			if(im.bound.quality == 0)
				out.srv = nullptr;
			return true;
		}
		return false;
	};
	if(!SnapBlock(live, tb))
	{
		if(live->seq == 0)
			m_state = State::Waiting;   // the section is made, nothing published yet
		return keepBound();
	}
	// the layout (checked on a published block: the producer fills the header before its first publication)
	{
		std::string why;
		if(tb.magic != VPSHELL_SHADOW_MAGIC || tb.version != VPSHELL_SHADOW_VERSION)
			why = Fmt("magic 0x%08X version %u, expected 0x%08X version %u", tb.magic, tb.version, VPSHELL_SHADOW_MAGIC,
			          VPSHELL_SHADOW_VERSION);
		else if(tb.headerBytes != sizeof(VPShellShadowTap) || tb.slotOffset != VPSHELL_SHADOW_SLOT_OFFSET ||
		        tb.slotBytes != sizeof(VPShellShadowSlot) || tb.slotCount != VPSHELL_SHADOW_SLOTS)
			why = Fmt("header %u bytes, slots %u x %u bytes at %u, expected %u, %u x %u at %u", tb.headerBytes, tb.slotCount,
			          tb.slotBytes, tb.slotOffset, (unsigned)sizeof(VPShellShadowTap), VPSHELL_SHADOW_SLOTS,
			          (unsigned)sizeof(VPShellShadowSlot), VPSHELL_SHADOW_SLOT_OFFSET);
		else if(tb.exePid != m_pid)
			why = Fmt("section pid %u, expected the EXE's %u", tb.exePid, m_pid);
		if(!why.empty())
		{
			refused++;
			m_failReason = VPSHDF_VERSION;
			m_lastWhy = "the shadow tap's section is refused by the server: " + why;
			m_state = State::Failed;
			RNote("shadow: LOUD: %s (VPSHDF_VERSION)", m_lastWhy.c_str());
			release();
			return false;
		}
	}
	if(tb.state != im.lastBlockState || tb.failReason != im.lastBlockFail)
	{
		RNote("shadow: tap block state %s (fail reason %s, pid %u, newest slot %d generation %u, quality %u, slot set %u, "
		      "%llu copies, %u missed, %u faults, %u description changes)",
		      tb.state < 4 ? kStateNames[tb.state] : "?", FailName(tb.failReason), tb.exePid, (int)tb.newestSlot, tb.generation,
		      tb.quality, tb.slotSet, (unsigned long long)tb.copiesTotal, live->missedTotal, live->faultsTotal,
		      live->descChanges);
		im.lastBlockState = tb.state;
		im.lastBlockFail = tb.failReason;
	}
	if(tb.state == VPSHD_FAILED)
	{
		tb.failText[VPSHELL_SHADOW_FAILTEXT - 1] = 0;
		m_failReason = tb.failReason;
		m_lastWhy = Fmt("the shadow tap could not install: VPSHD_FAILED %s (reason %u: %s)", FailName(tb.failReason),
		                tb.failReason, tb.failText[0] ? tb.failText : "shell_composite.log names the detail");
		RNote("shadow: LOUD: %s", m_lastWhy.c_str());
		m_state = State::Failed;
		release();
		return false;
	}
	if(tb.state != VPSHD_ACTIVE)
	{
		const State s = tb.state == VPSHD_OFF ? State::Off : State::Waiting;
		if(s != m_state && im.haveBound)
			RNote("shadow: tap %s: the bound triple (generation %u) is dropped", s == State::Off ? "OFF" : "not active",
			      im.bound.generation);
		m_state = s;
		if(im.haveBound || live->inUseSlot != VPSHELL_SHADOW_NO_SLOT)
		{
			im.haveBound = false;
			InterlockedExchange((volatile LONG*)&live->inUseSlot, (LONG)VPSHELL_SHADOW_NO_SLOT);
		}
		return false;
	}
	if(!im.dev1 && FAILED(m_dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&im.dev1)))
	{
		im.dev1 = nullptr;
		m_failReason = VPSHDF_SHARED;
		m_lastWhy = "the server's device has no ID3D11Device1 (OpenSharedResource1)";
		m_state = State::Failed;
		RNote("shadow: LOUD: %s", m_lastWhy.c_str());
		release();
		return false;
	}
	if(m_state != State::Active)
		m_state = im.haveBound ? State::Active : State::Waiting;

	// ---- staleness in map frames (the overlay's rule, issue 0194) ----
	const int64_t presents = m_hdr ? VPSHELL_CAMERA(m_hdr)->presentCount : 0;
	const uint64_t mapFrames = tb.copiesTotal + (uint64_t)live->missedTotal;
	if(tb.generation != im.lastGen)
	{
		if(im.staleEpisode)
		{
			const int64_t len = presents - im.lastGenPresent;
			if(len > staleLongestPresents)
				staleLongestPresents = len;
			if(stale <= (int64_t)SHADOW_READER_STALE_FULL_LINES)
				RNote("shadow: copies resumed (generation %u) after a stale episode of %lld Presents (%llu map frames)",
				      tb.generation, (long long)len, (unsigned long long)(mapFrames - im.lastGenMapFrames));
		}
		im.lastGen = tb.generation;
		im.lastGenPresent = presents;
		im.lastGenMapFrames = mapFrames;
		im.lastGenMs = GetTickCount64();
		im.staleEpisode = false;
		im.noMapGap = false;
	}
	else if(im.haveBound && !im.staleEpisode)
	{
		if(mapFrames < im.lastGenMapFrames)
			im.lastGenMapFrames = mapFrames;
		const uint64_t ms = GetTickCount64() - im.lastGenMs;
		const int64_t mf = (int64_t)(mapFrames - im.lastGenMapFrames);
		if(mf > (int64_t)SHADOW_READER_STALE_MAP_FRAMES && ms >= SHADOW_READER_STALE_MS)
		{
			im.staleEpisode = true;
			stale++;
			if(stale <= (int64_t)SHADOW_READER_STALE_FULL_LINES)
				RNote("shadow: stale (lag, counted, D132): no new copy for %lld map frames, %llu ms (generation %u, copy of "
				      "Present %llu; DLL counts %u missed, %u faults); the last triple stays bound",
				      (long long)mf, (unsigned long long)ms, tb.generation, (unsigned long long)im.bound.presentCount,
				      live->missedTotal, live->faultsTotal);
			if(stale == (int64_t)SHADOW_READER_STALE_FULL_LINES)
				RNote("shadow: further stale episodes are counted only");
		}
		else if(!im.noMapGap && presents - im.lastGenPresent > (int64_t)SHADOW_READER_STALE_MAP_FRAMES &&
		        ms >= SHADOW_READER_STALE_MS)
		{
			im.noMapGap = true;
			noMapGaps++;
			if(noMapGaps <= (int64_t)SHADOW_READER_STALE_FULL_LINES)
				RNote("shadow: %lld Presents, %llu ms, with %lld map frames since the last copy (frames without the "
				      "scene-clear list): not a lag, not counted as stale",
				      (long long)(presents - im.lastGenPresent), (unsigned long long)ms, (long long)mf);
		}
	}

	if(tb.generation == 0 || tb.newestSlot == VPSHELL_SHADOW_NO_SLOT ||
	   (im.haveBound && tb.generation == im.bound.generation))
		return keepBound();
	const uint32_t s = tb.newestSlot;
	auto refuse = [&](const std::string& why) -> bool {
		refused++;
		m_lastWhy = why;
		if(why != im.lastRefusal && im.refusedLogs++ < 32)
			RNote("shadow: LOUD: slot %u generation %u refused: %s%s", s, tb.generation, why.c_str(),
			      im.haveBound ? " (the last triple stays bound)" : "");
		im.lastRefusal = why;
		InterlockedExchange((volatile LONG*)&live->inUseSlot, (LONG)(im.haveBound ? im.bound.slot : VPSHELL_SHADOW_NO_SLOT));
		return keepBound();
	};
	if(s >= VPSHELL_SHADOW_SLOTS)
		return refuse(Fmt("newest slot %u of %u", s, VPSHELL_SHADOW_SLOTS));

	// the hand-over: name the slot, then check it is still the published copy
	InterlockedExchange((volatile LONG*)&live->inUseSlot, (LONG)s);
	MemoryBarrier();
	VPShellShadowSlot sl;
	if(!SnapSlot(VPSHELL_SHADOWSLOT(im.sec, s), sl) || !sl.valid || sl.generation != tb.generation)
	{
		// torn or moved on (a newer copy, or the producer is writing this slot): never taken; the last triple this time
		InterlockedExchange((volatile LONG*)&live->inUseSlot, (LONG)(im.haveBound ? im.bound.slot : VPSHELL_SHADOW_NO_SLOT));
		return keepBound();
	}
	if(sl.numViews == 0 || sl.numViews > VPSHELL_SHADOW_MAX_VIEWS)
		return refuse(Fmt("numViews %u (1..%u)", sl.numViews, VPSHELL_SHADOW_MAX_VIEWS));
	for(uint32_t v = 0; v < sl.numViews; ++v)
		for(int k = 0; k < 16; ++k)
			if(!isfinite(sl.shadowMatrix0[v][k]))
				return refuse(Fmt("ShadowMatrix0 view %u element %d is not finite", v, k));
	if(!isfinite(sl.strength) || sl.strength < 0.0f || sl.strength > 1.0f)
		return refuse(Fmt("strength %g (finite, 0..1)", sl.strength));
	if(sl.quality > 3)
		return refuse(Fmt("quality %u (0..3)", sl.quality));
	{
		const std::string d = DescProblem(sl);
		if(!d.empty())
			return refuse(d);
	}
	if(sl.presentCount == 0)
		return refuse("presentCount 0");

	ID3D11ShaderResourceView* srv = nullptr;
	if(sl.quality != 0)
	{
		Impl::Tex& tx = im.tex[s];
		if(!tx.tex || tx.epoch != sl.texEpoch || tx.pid != tb.exePid)
		{
			if(im.haveBound && im.bound.slot == s)
				im.haveBound = false;   // its SRV goes now
			im.dropTex(s);
			wchar_t name[96];
			_snwprintf_s(name, _countof(name), _TRUNCATE, VPSHELL_SHADOW_TEXTURE_FMT, tb.exePid, s, sl.texEpoch);
			ID3D11Texture2D* t = nullptr;
			VPSharedOpen how;
			HRESULT hr = VPOpenSharedTexture(im.dev1, tb.exePid, sl.ntHandle, name, DXGI_SHARED_RESOURCE_READ, &t, &how);
			if(FAILED(hr) || !t)
			{
				char why[400];
				return refuse(VPSharedFailText(why, sizeof(why), how, name, tb.exePid, sl.ntHandle));
			}
			VPShellShadowSlot again;
			if(how.via == VPSHARED_HANDLE && (!SnapSlot(VPSHELL_SHADOWSLOT(im.sec, s), again) || again.texEpoch != sl.texEpoch ||
			                                  again.ntHandle != sl.ntHandle))
			{
				// the DLL made the slot's texture again while we opened it: the value may name the new one; next frame
				t->Release();
				InterlockedExchange((volatile LONG*)&live->inUseSlot, (LONG)(im.haveBound ? im.bound.slot : VPSHELL_SHADOW_NO_SLOT));
				return keepBound();
			}
			D3D11_TEXTURE2D_DESC d;
			t->GetDesc(&d);
			if(d.Width != sl.width || d.Height != sl.height || d.MipLevels != 1 || d.ArraySize != 1 || d.SampleDesc.Count != 1 ||
			   (uint32_t)d.Format != sl.dxgiFormat)
			{
				t->Release();
				return refuse(Fmt("shared texture '%ls' is %u x %u, %u mips, array %u, format %u; the slot describes %u x %u, "
				                  "format %u",
				                  name, d.Width, d.Height, d.MipLevels, d.ArraySize, (unsigned)d.Format, sl.width, sl.height,
				                  sl.dxgiFormat));
			}
			ID3D11ShaderResourceView* v = nullptr;
			hr = m_dev->CreateShaderResourceView(t, nullptr, &v);   // the texture's own format: R16_UNORM or RGBA16F
			if(FAILED(hr) || !v)
			{
				t->Release();
				return refuse(Fmt("CreateShaderResourceView on '%ls' failed 0x%08X", name, (unsigned)hr));
			}
			tx.tex = t;
			tx.srv = v;
			tx.epoch = sl.texEpoch;
			tx.pid = tb.exePid;
			reopened++;
			RNote("shadow: slot %u texture '%ls' opened %s (%u x %u, format %u, quality %u)", s, name, VPSharedViaText(how), d.Width,
			      d.Height, (unsigned)d.Format, sl.quality);
		}
		srv = tx.srv;
		// verify (diagnostic): the EXE texture's hash at the copy's Present against this device's read of the shared copy
		if(sl.flags & VPSHD_SLOT_VERIFY)
		{
			D3D11_TEXTURE2D_DESC d;
			tx.tex->GetDesc(&d);
			if(!im.staging || im.stagingDesc.Width != d.Width || im.stagingDesc.Height != d.Height || im.stagingDesc.Format != d.Format)
			{
				SafeRelease(im.staging);
				D3D11_TEXTURE2D_DESC sd = d;
				sd.Usage = D3D11_USAGE_STAGING;
				sd.BindFlags = 0;
				sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				sd.MiscFlags = 0;
				if(FAILED(m_dev->CreateTexture2D(&sd, nullptr, &im.staging)))
					im.staging = nullptr;
				im.stagingDesc = d;
			}
			uint64_t h = VPSHADOW_FNV_INIT, bytes = 0;
			bool ok = false;
			ID3D11DeviceContext* ctx = nullptr;
			m_dev->GetImmediateContext(&ctx);
			if(im.staging && ctx)
			{
				ctx->CopyResource(im.staging, tx.tex);
				D3D11_MAPPED_SUBRESOURCE m;
				if(SUCCEEDED(ctx->Map(im.staging, 0, D3D11_MAP_READ, 0, &m)))
				{
					const size_t row = (size_t)d.Width * BytesPerTexel((uint32_t)d.Format);
					for(uint32_t y = 0; y < d.Height; ++y)
						h = VPShadowFnv(h, (const uint8_t*)m.pData + (size_t)y * m.RowPitch, row);
					bytes = (uint64_t)row * d.Height;
					ctx->Unmap(im.staging, 0);
					ok = true;
				}
			}
			SafeRelease(ctx);
			const bool same = ok && h == sl.verifyHash && bytes == sl.verifyBytes;
			(same ? verifyOk : verifyBad)++;
			RNote("shadow: VERIFY generation %u (slot %u, quality %u, Present %llu): %llu bytes, server fnv %016llX, EXE fnv %016llX: "
			      "%s (totals %lld equal, %lld differ)",
			      sl.generation, s, sl.quality, (unsigned long long)sl.presentCount, (unsigned long long)bytes,
			      (unsigned long long)h, (unsigned long long)sl.verifyHash, same ? "EQUAL" : (ok ? "DIFFERS" : "READ FAILED"),
			      (long long)verifyOk, (long long)verifyBad);
		}
	}
	// bound: ack
	InterlockedExchange((volatile LONG*)&live->ackServerPid, (LONG)GetCurrentProcessId());
	MemoryBarrier();
	InterlockedExchange((volatile LONG*)&live->ackGeneration, (LONG)sl.generation);
	ShadowFrame f;
	f.format = sl.quality ? sl.dxgiFormat : 0;
	f.width = sl.quality ? sl.width : 0;
	f.height = sl.quality ? sl.height : 0;
	f.numViews = sl.numViews;
	memcpy(f.matrices, sl.shadowMatrix0, sizeof(f.matrices));
	f.strength = sl.strength;
	f.presentCount = sl.presentCount;
	f.generation = sl.generation;
	f.quality = sl.quality;
	f.slot = s;
	f.unitMaskDropped = (sl.flags & VPSHD_SLOT_UNIT_MASK_DROPPED) != 0;   // ticket 4785
	f.rawWidth = sl.rawWidth;
	f.rawHeight = sl.rawHeight;
	if(!im.haveBound)
		RNote("shadow: first triple bound: slot %u generation %u, copy of Present %llu, quality %u, %u x %u format %u, %u views, "
		      "strength %g",
		      s, sl.generation, (unsigned long long)sl.presentCount, sl.quality, f.width, f.height, f.format, sl.numViews,
		      sl.strength);
	else if(im.bound.quality != f.quality || im.bound.width != f.width || im.bound.format != f.format)
		RNote("shadow: the bound description changed: quality %u %u x %u format %u -> quality %u %u x %u format %u (generation %u)",
		      im.bound.quality, im.bound.width, im.bound.height, im.bound.format, f.quality, f.width, f.height, f.format,
		      sl.generation);
	im.bound = f;
	im.haveBound = true;
	m_state = State::Active;
	accepted++;
	out = f;
	out.srv = srv;
	return true;
}

}  // namespace srvrender
