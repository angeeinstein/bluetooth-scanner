#pragma once

// Shared secret embedded in every legitimate firmware build. Before installing
// an uploaded image, the device scans it for this exact byte sequence and
// rejects the update if it's absent -- so a random/unrelated firmware (which
// won't contain the token) can't be flashed by someone who merely reaches the
// dashboard. No manual password: the "authentication" travels inside the .bin.
//
// This is obscurity, NOT real security -- the token is plaintext in the binary,
// so anyone who obtains a build, dumps a device's flash, or sniffs an upload can
// extract it. It stops the casual/accidental case (the stated goal), not a
// determined attacker; that would need Secure Boot signing.
//
// Rotate it any time by pasting a new random value here and rebuilding (e.g.
//   python -c "import secrets; print('VSK1-' + secrets.token_hex(20))"
// ). Keep this value -- and your .bin files -- private. Set to "" to disable
// the check (any valid firmware image will then be accepted). A USB flash always
// bypasses this check, so a mismatched token can never lock you out.
#define OTA_UPDATE_TOKEN "VSK1-721aef63637dc82608aeb56e5af851241d969086"
