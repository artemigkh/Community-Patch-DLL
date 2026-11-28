// ring_source.h - the ring source (issue 3302): the consumer side of vp_frame.h, x86, inside the composite DLL (3304
// integrates it in wave 2) or the stand-in consumer. Implements vpframe::FrameSource (frame_source.h) exactly; the
// extra members below are optional (logging, counters for tests and the composite's per-second line).
#pragma once
#include "frame_source.h"
#include "ring_common.h"

struct ID3D11Texture2D;
struct ID3D11Device1;
struct ID3D11Device5;
struct ID3D11DeviceContext4;
struct ID3D11Fence;
struct ID3D11Query;

namespace vpframe {

struct RingSourceStats
{
	int64_t exact = 0, older = 0, none = 0;   // acquire() results
	int64_t claimRetries = 0;                // a slot claim lost the Dekker race (server took the slot) and fell back
	int64_t released = 0;                    // slot uses whose event query completed (slotReleased written)
	int64_t generations = 0;                 // ring generations opened
	int64_t serverChanges = 0;               // a new server pid (restart) or the server gone
	int64_t openFailures = 0;                // a generation's textures or fence could not be opened
	int64_t refusedSection = 0;              // acquire() refused: bad magic/version/slot count/size/adapter
	int64_t fenceWaits = 0;                  // ID3D11DeviceContext4::Wait queued
	int64_t pendingMax = 0;                  // most slot uses in flight at once
	int64_t fenceDoneAtAcquire = 0;          // FENCE: the slot's value had already completed when it was taken
	int64_t fenceCompletedMax = 0;           // the highest completed value seen (diagnostics)
	int64_t fencePoisoned = 0;               // generations whose fence was found completed to UINT64_MAX
};

class RingSource : public FrameSource
{
public:
	// slots: VPFRAME_MIN_SLOTS..VPFRAME_MAX_SLOTS (clamped); sync: VPSYNC_FENCE (falls back to CPU when the device has no
	// ID3D11Device5 / ID3D11DeviceContext4 or the fence cannot be opened) or VPSYNC_CPU.
	explicit RingSource(uint32_t slots = VPFRAME_DEFAULT_SLOTS, uint32_t sync = VPSYNC_FENCE);
	~RingSource() override;
	bool open(ID3D11Device* dev, ID3D11DeviceContext* ctx, const wchar_t* baseName) override;
	void request(uint32_t width, uint32_t height) override;
	int64_t newest() override;
	bool waitFor(int64_t frameId, int64_t deadlineQpc) override;
	Take acquire(ID3D11DeviceContext* ctx, int64_t frameId, SourceFrame& out) override;
	void release(ID3D11DeviceContext* ctx, const SourceFrame& f) override;
	void poll(int64_t nowQpc) override;
	void close() override;
	const char* name() const override { return "ring"; }

	// ---- beyond frame_source.h (optional) ----
	void setLog(ring::LogFn fn, void* user) { m_log.fn = fn; m_log.user = user; }
	const RingSourceStats& stats() const { return m_stats; }
	uint32_t generation() const { return m_gen; }
	uint32_t syncInForce() const { return m_gen ? m_ringSync : 0; }
	const void* section() const { return m_sec; }
	int pending() const { return m_pendN; }

private:
	bool serverOk();
	bool ensureRing();
	void dropRing();
	void writeRequest();
	void complete();

	ring::Logger m_log;
	RingSourceStats m_stats;
	uint32_t m_wantSlots, m_wantSync, m_syncOverride = 0;
	ID3D11Device* m_dev = nullptr;
	ID3D11DeviceContext* m_ctx = nullptr;
	ID3D11Device1* m_dev1 = nullptr;
	ID3D11Device5* m_dev5 = nullptr;
	ID3D11DeviceContext4* m_ctx4 = nullptr;
	uint64_t m_luid = 0;
	HANDLE m_map = nullptr;
	void* m_sec = nullptr;
	HANDLE m_frm = nullptr, m_cam = nullptr, m_timer = nullptr;
	wchar_t m_base[200] = {};
	DWORD m_owner = 0;                 // the thread that opened (the EXE's main thread): only it touches the context
	SRWLOCK m_lock = SRWLOCK_INIT;     // the server-process handle (poll() may run on the composite's control thread)
	HANDLE m_srvProc = nullptr;
	uint32_t m_srvProcPid = 0;
	volatile LONG m_srvGone = 0;

	uint32_t m_reqW = 0, m_reqH = 0, m_reqSent = 0;
	// the opened generation
	uint32_t m_gen = 0, m_genPid = 0, m_ringSeq = 0, m_ringSync = 0, m_slotCount = 0, m_w = 0, m_h = 0;
	uint32_t m_failedGen = 0, m_failedPid = 0;   // a generation that could not be opened (not retried)
	int64_t m_nextTryQpc = 0;
	ID3D11Texture2D* m_colTex[VPFRAME_MAX_SLOTS] = {};
	ID3D11Texture2D* m_depTex[VPFRAME_MAX_SLOTS] = {};
	ID3D11ShaderResourceView* m_colSrv[VPFRAME_MAX_SLOTS] = {};
	ID3D11ShaderResourceView* m_depSrv[VPFRAME_MAX_SLOTS] = {};
	ID3D11Fence* m_fence = nullptr;

	// slot uses in flight (event query per use, FIFO: the GPU completes them in order)
	struct Pending { ID3D11Query* q; int slot; int64_t frameId; };
	enum { kPend = 32 };
	Pending m_pend[kPend] = {};
	int m_pendHead = 0, m_pendN = 0;
	ID3D11Query* m_pool[kPend] = {};
	int m_poolN = 0;
	// Claims per slot not yet complete (acquired, and not yet released with a completed query). slotReleased[s] is
	// written only when this reaches 0: the same frame can be taken again (a miss) while its earlier use is in flight,
	// and both uses carry one frame id.
	int m_out[VPFRAME_MAX_SLOTS] = {};
	int64_t m_claimSeq = 0;            // the value of the newest claim (slotInUse); reset with the claims at open()
	uint32_t m_poisonGen = 0;          // the generation whose fence was found completed to UINT64_MAX
	uint32_t m_logged = 0;             // one bit per refusal kind logged once
	int m_openedVia = 0;               // VPSharedVia of the open generation's first texture (its log line)
};

}  // namespace vpframe
