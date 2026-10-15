#pragma once

// The hub's side of the upscaler bridge connection (see ../upscaler_bridge_protocol.h): opens the shared objects a
// plugin created with UpscalerBridge::Client, answers its start and serves its requests until it asks to stop or its
// process ends. No GPU and no vendor SDK here: upscaler_bridge.cpp does the D3D12, DLSS and FSR work for each request.
// Separate from it so that the handshake can be tested without a GPU (tests/upscaler.cpp).

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <string>
#include "../upscaler_bridge_protocol.h"

namespace UpscalerBridge::Server
{
    enum class OpenResult
    {
        Ok,
        MissingObjects,     // no mapping or events of that name (the plugin is gone or the name is wrong)
        VersionMismatch,    // the plugin speaks another protocol version: no answer, the hub ends, the plugin sees it end
        NoClient,           // the plugin's process can't be opened
    };

    struct Connection
    {
        HANDLE mapping = nullptr;
        HANDLE request = nullptr;
        HANDLE response = nullptr;
        HANDLE client = nullptr;
        Shared* shared = nullptr;
        void (*log)(const char* line) = nullptr;    // Message() also goes here

        Connection() = default;
        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;

        ~Connection()
        {
            Close();
        }

        void Close()
        {
            if (shared)
                UnmapViewOfFile(shared);
            for (auto handle : { mapping, request, response, client })
                if (handle)
                    CloseHandle(handle);
            shared = nullptr;
            mapping = request = response = client = nullptr;
        }

        OpenResult Connect(const std::wstring& name)
        {
            mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, (name + MappingSuffix).c_str());
            request = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, (name + RequestSuffix).c_str());
            response = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, (name + ResponseSuffix).c_str());
            if (!mapping || !request || !response)
                return OpenResult::MissingObjects;

            shared = static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
            if (!shared)
                return OpenResult::MissingObjects;
            if (shared->Version != Version)
                return OpenResult::VersionMismatch;

            client = OpenProcess(SYNCHRONIZE | PROCESS_DUP_HANDLE, FALSE, shared->ClientProcessId);
            if (!client)
                return OpenResult::NoClient;

            // The strings come from the plugin: always terminated
            shared->LogPath[PathLength - 1] = 0;
            shared->GameDirectory[PathLength - 1] = 0;
            shared->ClientName[sizeof(shared->ClientName) - 1] = 0;
            return OpenResult::Ok;
        }

        bool Open(const std::wstring& name)
        {
            return Connect(name) == OpenResult::Ok;
        }

        // Answers the current request (or the start), exactly once
        void Respond(Status status)
        {
            shared->ResponseStatus = status;
            MemoryBarrier();
            shared->ResponseSerial = shared->RequestSerial;
            MemoryBarrier();
            SetEvent(response);
        }

        void Message(const char* format, ...)
        {
            va_list args;
            va_start(args, format);
            vsnprintf(shared->Message, sizeof(shared->Message), format, args);
            va_end(args);
            if (log)
                log(shared->Message);
        }

        // Serves requests until Shutdown (answered Ok, returns true) or until the plugin's process ends or the wait
        // fails (returns false). handler(Command) -> bool answers every other command, Ok when it returns true.
        template<typename Handler>
        bool Serve(Handler&& handler)
        {
            HANDLE handles[] = { request, client };
            while (true)
            {
                auto wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                if (wait != WAIT_OBJECT_0)
                    return false;

                MemoryBarrier();
                auto command = shared->RequestCommand;
                if (command == Command::Shutdown)
                {
                    Respond(Status::Ok);
                    return true;
                }

                bool ok = handler(command);
                Respond(ok ? Status::Ok : Status::Failed);
            }
        }
    };
}
