/*
        GTA V Free Camera / Photo Mode Plugin
        Minimal append-only log

        Exists for one reason: a crash or a silent load failure under FiveM used
        to leave nothing behind at all, so "it did not work" was the entire bug
        report. Every line is flushed immediately — a log that buffers tells you
        nothing about the thing that killed the process.

        Writes SimpleCamera.log next to the .asi. Never throws, never allocates,
        and is safe to call before anything else is initialised.
*/

#pragma once

void Log(const char *fmt, ...);
