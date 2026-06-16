// OhMyDisplay - elevated helper IPC over a local named pipe.
//
// Topology: the non-elevated MAIN process is the pipe SERVER; the elevated helper is
// the CLIENT. (A higher-integrity client may open a lower-integrity server pipe of the
// same user, so no special ACL is needed; we still verify the peer image path.)
//
// Framing: each message is a 4-byte little-endian length followed by that many UTF-8
// bytes of JSON.
//
// Request/response JSON:
//   {"cmd":"collect"}                       -> {"ok":true,"requests":"...","waketimers":"..."}
//   {"cmd":"devicedisablewake","device":X}  -> {"ok":true,"exitCode":0}
//   on error                                -> {"ok":false,"error":"..."}
#pragma once

#include "core/json.h"

#include <windows.h>
#include <string>

namespace omd
{
    // Low-level framed message I/O (shared by both sides).
    bool IpcWriteMessage(HANDLE pipe, const std::string& payload);
    bool IpcReadMessage(HANDLE pipe, std::string& payload, DWORD maxBytes = (1u << 20));

    // MAIN side: create a unique server pipe, launch the elevated helper as a client,
    // exchange one request/response. Returns false if UAC was declined or IPC failed.
    bool ElevatedRequest(const json::Value& request, json::Value& response);

    // HELPER side: connect to the server pipe, serve exactly one request, then return.
    // Returns process exit code (0 on success).
    int ElevatedServe(const std::wstring& pipeName);
}
