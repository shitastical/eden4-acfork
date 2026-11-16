// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Every file this fork owns lives under one root. A normal Eden4 install keeps using
// /data/EdenPS4, so the two never share settings, keys, saves or shader caches and can be
// installed side by side.
//
// This fork is deliberately single-game: the only thing it looks for is ACNH, and the layout
// is organised so each kind of data is easy to find over FTP:
//
//   /data/ACNH/
//     ps4.ini            fork settings (hand-editable, same keys as the launcher's menu)
//     ui-state.ini       home-menu cursor position
//     games/             put the Animal Crossing NSP/XCI here
//     keys/              prod.keys, title.keys
//     nand/              save data, system titles, firmware
//     cache/shader/      translated GPU shader cache (survives reinstalls)
//     cache/ui/          cached menu icons/metadata
//     logs/              eden.log, frames.log, hang.log, crash.log
namespace Ps4Paths {

inline constexpr char Root[] = "/data/ACNH";
inline constexpr char Games[] = "/data/ACNH/games";
inline constexpr char Keys[] = "/data/ACNH/keys";
inline constexpr char Nand[] = "/data/ACNH/nand";
inline constexpr char Cache[] = "/data/ACNH/cache";
inline constexpr char ShaderCache[] = "/data/ACNH/cache/shader";
inline constexpr char UiCache[] = "/data/ACNH/cache/ui";
inline constexpr char Logs[] = "/data/ACNH/logs";
inline constexpr char Ini[] = "/data/ACNH/ps4.ini";
inline constexpr char UiState[] = "/data/ACNH/ui-state.ini";
inline constexpr char HangLog[] = "/data/ACNH/logs/hang.log";
inline constexpr char Log[] = "/data/ACNH/logs/eden.log";
inline constexpr char FrameLog[] = "/data/ACNH/logs/frames.log";

/// Title id of Animal Crossing: New Horizons (US v0). This fork targets this one game.
inline constexpr unsigned long long GameTitleId = 0x01006F8002326000ULL;

} // namespace Ps4Paths
