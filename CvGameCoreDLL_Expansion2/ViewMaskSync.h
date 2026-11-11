/*	-------------------------------------------------------------------------------------------------------
	ViewMaskSync - make the EXE's per-view system mask honour config.ini [DisableSystems], opt-in.

	The EXE keeps two masks of world systems (bit set = system on):
	  config mask  [app+0x940], app = g_Civ5App (CivilizationV_DX11.exe VA 0xCC8E74). Each [DisableSystems]
	               key set to 1 clears one bit; it gates the systems' Startup calls.
	  view mask    [GVS+0x1B4], GVS = the GameViewState at [app+0xC0]. Its constructor writes 0xFFFFFFFF and
	               the per-game startups clear only some bits; the per-frame update and the render passes test it.
	So a system disabled in config.ini whose view bit stays set is updated and rendered without having been
	started: River (0x100) and Arrows (0x400000) crash at the end of the load (VA 0x8CD492), Combat, Projectile,
	YieldIcons and DistanceFog keep running.

	This instrument ANDs the view mask with the config mask (the 24 known bits only; the top byte is left as
	it is), from DllGetGameContext and again from every CvGame::update while a bit is set that the config mask
	clears (the per-game startups OR some bits back, e.g. 0x1800 at VA 0x62344B; a new GameViewState starts at
	0xFFFFFFFF). It is a write to EXE heap data; no EXE code byte is touched.

	Switch: crashlogs\viewmask.enabled in the install folder (the working directory). Optional content
	"keep=0x<hex>": bits that are never cleared, re-read every 2 s; bits added to it later are set back if this
	instrument cleared them (never River or Arrows). "clear=0x<hex>" (anywhere in the file, also re-read every
	2 s): bits that are cleared even though the config keeps them; it wins over the config and over "keep=".
	For D7 option c1: Terrain stays on in config.ini (so the per-load bake fills the height field and the
	terrain decal system is started) and clear=0x1 stops the terrain update and draw. GVS::StartupGame ORs bit 0
	back at every load (VA 0x468BCC); the CvGame::update re-check clears it again, so the frames rendered between
	that startup and the first CvGame::update after it still have the bit set (load screen). Bits taken out of
	"clear=" later are set back if the config allows them. Removing the marker while the game runs sets back every
	bit it cleared except River and Arrows and stops it. Without the marker nothing is done and nothing is logged.

	Checks before any write: PE TimeDateStamp 0x546CD0A8 (CivilizationV_DX11.exe, Steam build 4390913), six
	code patterns that read or write the two masks, the g_Civ5App address inside two of them, the GameViewState
	vtable (VA 0xA20908), and that every pointer followed lies in committed readable (for the mask: writable)
	memory. Anything else: one "off:" or "skip:" line in crashlogs\viewmask.log and no write.
	Log: crashlogs\viewmask.log (appended), every change with the values before and after, and one read-back
	line per game turn.

	White FOW-mask stand-in (test branch i2-white-mask, c5-64bit-patch issue 3011, decision D56): marker content
	"whitemask=1" (read once, at Install). With FluidFOW off the FOW mask texture handle (VA 0xDA12AC) stays 0 and the
	building, improvement, resource and wonder shaders draw the fog base layer instead of the lit model (dark red),
	and the forest shader discards every tree. Install (DllGetGameContext, the EXE's main thread, front end) then
	checks the loader VA 0x8C6540, the terrain editor's call of it at VA 0x561FCB (which loads the same file into the
	same global) and the building draw-object builder's read at VA 0x7B267C; requires the config mask to have
	FluidFOW (0x8000) off, the renderer (VA 0x3010570) and the device (VA 0x301057C) to exist and the mask global to
	be 0; then calls the loader once (__thiscall, ECX = [0x3010570], one argument: the path; EAX = handle) for
	"assets\terrain\editor\White.dds" and writes the handle into 0xDA12AC with a compare-and-swap. The EXE's code is
	called, never changed. The texture is owned by this DLL for the life of the process and never released.
	CvGame::update (game-core thread) only reads the global once per turn and, if it is 0 again, writes the cached
	handle back; it never calls the loader. Removing the marker does not take the white mask out.

	Research: c5-64bit-patch research/02-exe-internals/disable-systems-and-masks.md (sections 3 and 4; the white mask:
	section 5, "Why the models are dark red in D33").
	------------------------------------------------------------------------------------------------------- */

#pragma once

#ifndef CIV5_VIEW_MASK_SYNC_H
#define CIV5_VIEW_MASK_SYNC_H

namespace ViewMaskSync
{
//! Reads the marker, verifies the EXE and applies the mask once if the GameViewState exists. Only the first
//! call does anything. From DllGetGameContext.
void Install();
//! Re-reads the marker every 2 s and re-applies the mask when a bit the config mask (or "clear=") clears is set
//! again.
//! Cheap when nothing changed. From the top of CvGame::update (game-core thread).
void OnGameUpdate(int iGameTurn);
}

#endif // CIV5_VIEW_MASK_SYNC_H
