// shell_shared.h - layout of the named shared-memory section that links
// shell_server.exe (x64, producer) and shell_composite.dll (x86, consumer).
//
// The layout uses only fixed-size fields with explicit padding so the 32-bit
// and the 64-bit compiler agree on every offset (checked by static_asserts).
//
// Concurrency rules
//   * The "texture block" (serverPid .. textureName) is written by the server
//     under a seqlock: texSeq odd while writing, even when stable. The client
//     copies the block and accepts it only if texSeq was even and unchanged.
//   * The "frame block" (frameCounter, frameQpc) is written by the server while
//     it holds the texture's keyed mutex, right before ReleaseSync(1). The
//     client reads it right after AcquireSync(1) succeeded, so it always
//     describes exactly the pixels the client is about to copy.
//   * heartbeatQpc is written every server loop iteration (64-bit atomic).
//   * The "request block" is written by the client: reqWidth/reqHeight first,
//     then reqSeq is incremented. The server answers by recreating the texture
//     and writing ackSeq = reqSeq.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define VPSHELL_SECTION_DEFAULT L"Local\\VPShellSection"
#define VPSHELL_TEXTURE_BASE_DEFAULT L"Local\\VPShellWorld"
#define VPSHELL_MAGIC 0x4C485356u /* 'VSHL' */
#define VPSHELL_VERSION 1u
#define VPSHELL_SECTION_BYTES 4096u

#pragma pack(push, 8)
struct VPShellHeader
{
	uint32_t magic;            // VPSHELL_MAGIC once the server initialised the section
	uint32_t version;          // VPSHELL_VERSION

	// ---- texture block (seqlock) ----
	volatile uint32_t texSeq;  // odd = being written
	uint32_t serverPid;        // identifies a server instance (restart => new pid)
	uint32_t generation;       // incremented every time the texture is recreated
	uint32_t width;
	uint32_t height;
	uint32_t dxgiFormat;       // DXGI_FORMAT of the shared texture (BGRA8 UNORM)
	uint64_t adapterLuid;      // LUID of the adapter the server renders on (High<<32 | Low)
	wchar_t textureName[128];  // name for ID3D11Device1::OpenSharedResourceByName

	// ---- frame block (keyed mutex) ----
	volatile int64_t frameCounter;  // frames published since the server started
	volatile int64_t frameQpc;      // QPC when the last published frame was released (CPU submit time)

	// ---- liveness ----
	volatile int64_t heartbeatQpc;  // QPC of the server's last loop iteration
	int64_t qpcFrequency;           // QueryPerformanceFrequency of the server (same machine => same)
	volatile int64_t framesOverwritten; // frames the server replaced before the client consumed them

	// ---- request block (client -> server) ----
	volatile uint32_t reqSeq;
	volatile uint32_t reqWidth;
	volatile uint32_t reqHeight;
	volatile uint32_t ackSeq;       // server: last reqSeq it acted on
	volatile uint32_t clientPid;
	uint32_t pad1;
	volatile uint64_t clientLuid;   // adapter LUID of the client's device (diagnostics)
	volatile int64_t clientFramesConsumed;
};
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(VPShellHeader) <= VPSHELL_SECTION_BYTES, "header too big");
static_assert(offsetof(VPShellHeader, adapterLuid) == 32, "layout");
static_assert(offsetof(VPShellHeader, frameCounter) == 40 + 256, "layout");
static_assert(sizeof(VPShellHeader) == 376, "layout");
#endif
