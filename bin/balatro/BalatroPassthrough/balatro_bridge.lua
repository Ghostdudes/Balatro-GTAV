-- balatro_bridge.lua
-- Passthrough IPC bridge between Balatro (LÖVE2D) and Grand Theft Auto V.

-- Check if Balatro was launched with GTA passthrough enabled
local is_passthrough = false

-- 1. Check environment variable
local env_var = os.getenv("BALATRO_GTA_PASSTHROUGH")
if env_var and (env_var == "1" or env_var == "true") then
    is_passthrough = true
end

-- 2. Check command line arguments passed to Balatro (Love2D global arg table)
if arg and type(arg) == "table" then
    for i = 0, #arg do
        local a = tostring(arg[i])
        if a == "--gta-passthrough" or a == "-gta" or a == "--gta" then
            is_passthrough = true
            break
        end
    end
end

if not is_passthrough then
    -- Normal game launch from Steam / desktop:
    -- Do NOT activate the bridge, do NOT mute audio, do NOT throttle FPS, do NOT open shared memory.
    return {}
end

local ffi = require("ffi")

local log_path = "C:\\Users\\micrs\\AppData\\Roaming\\Balatro\\bridge_debug.log"
local function log(msg)
    local f = io.open(log_path, "a")
    if f then
        f:write(os.date("%X") .. " " .. tostring(msg) .. "\n")
        f:close()
    end
end

log("balatro_bridge module loaded (GTA Passthrough Mode Active)")

ffi.cdef[[
    typedef void* HANDLE;
    typedef unsigned long DWORD;
    typedef int BOOL;
    typedef size_t SIZE_T;
    typedef intptr_t intptr_t;

    HANDLE CreateFileMappingA(HANDLE hFile, void* lpAttributes, DWORD flProtect, DWORD dwMaxSizeHigh, DWORD dwMaxSizeLow, const char* lpName);
    HANDLE OpenFileMappingA(DWORD dwDesiredAccess, BOOL bInheritHandle, const char* lpName);
    void* MapViewOfFile(HANDLE hFileMappingObject, DWORD dwDesiredAccess, DWORD dwFileOffsetHigh, DWORD dwFileOffsetLow, SIZE_T dwNumberOfBytesToMap);
    BOOL UnmapViewOfFile(const void* lpBaseAddress);
    BOOL CloseHandle(HANDLE hObject);
    DWORD GetCurrentProcessId();
    uint64_t GetTickCount64();
    void* VirtualAlloc(void* lpAddress, size_t dwSize, DWORD flAllocationType, DWORD flProtect);

    struct InputEvent {
        uint16_t type;
        uint16_t code;
        float    x;
        float    y;
        uint32_t extra;
        uint32_t pad[2];
    };
]]

-- Create 6-byte x86_64 JIT thunk: lock xchg [rcx], edx; mov eax, edx; ret
local thunk_mem = ffi.C.VirtualAlloc(nil, 64, 0x3000, 0x40) -- MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE
if thunk_mem ~= nil then
    local thunk_code = ffi.new("uint8_t[6]", { 0xF0, 0x87, 0x11, 0x89, 0xD0, 0xC3 })
    ffi.copy(thunk_mem, thunk_code, 6)
end
local atomic_xchg = ffi.cast("uint32_t (*)(uint32_t*, uint32_t)", thunk_mem)

local MAX_W = 3840
local MAX_H = 2160
local SLOT_BYTES = MAX_W * MAX_H * 4
local OFF_PIXELS = 0x10000
local MAPPING_BYTES = OFF_PIXELS + (SLOT_BYTES * 3)

local BalatroBridge = {
    initialized = false,
    mapping_handle = nil,
    base_ptr = nil,
    mapping_bytes = MAPPING_BYTES,
    
    back_slot = 0,
    frame_id = 0,
    
    cursor_x = 0,
    cursor_y = 0,
    norm_x = 0.5,
    norm_y = 0.5,
    cursor_active = false,
    
    mouse_down = { false, false, false },
    last_capture_time = 0,
    
    logged_update = false,
    logged_draw = false,
    is_muted = false,
    saved_volume = 1.0,
}

local FILE_MAP_ALL_ACCESS = 0xF001F
local PAGE_READWRITE = 0x04
local INVALID_HANDLE_VALUE = ffi.cast("HANDLE", ffi.cast("intptr_t", -1))

local OFF_HEADER         = 0x0
local OFF_BALATRO_STATE  = 0x100
local OFF_GTA_STATE      = 0x180
local OFF_OVERLAY_CTL    = 0x200
local OFF_OVERLAY_SLOTS  = 0x240
local OFF_INPUT_RING     = 0x1000

local INPUT_RING_ENTRIES = 512

function BalatroBridge.init()
    if BalatroBridge.initialized then return end
    log("BalatroBridge.init() called")

    local ok, err = pcall(function()
        local map_name = "Local\\BalatroGTA_v1"
        local hMap = ffi.C.OpenFileMappingA(FILE_MAP_ALL_ACCESS, 0, map_name)
        if hMap == nil then
            hMap = ffi.C.CreateFileMappingA(INVALID_HANDLE_VALUE, nil, PAGE_READWRITE, 0, BalatroBridge.mapping_bytes, map_name)
        end

        log("CreateFileMappingA returned: " .. tostring(hMap))
        if hMap == nil then
            error("Failed to create file mapping: " .. map_name)
        end

        local ptr = ffi.C.MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, BalatroBridge.mapping_bytes)
        log("MapViewOfFile returned: " .. tostring(ptr))
        if ptr == nil then
            ffi.C.CloseHandle(hMap)
            error("Failed to map view of file")
        end

        BalatroBridge.mapping_handle = hMap
        BalatroBridge.base_ptr = ffi.cast("uint8_t*", ptr)
        BalatroBridge.initialized = true

        -- Initialize Header
        local p32 = ffi.cast("uint32_t*", BalatroBridge.base_ptr + OFF_HEADER)
        p32[0] = 0x42414C54 -- "BALT"
        p32[1] = 1          -- version
        p32[3] = ffi.C.GetCurrentProcessId() -- balatroPid

        -- Intercept love.mouse functions
        local orig_mouse_get_position = love.mouse.getPosition
        love.mouse.getPosition = function()
            if BalatroBridge.cursor_active then
                return BalatroBridge.cursor_x, BalatroBridge.cursor_y
            end
            return orig_mouse_get_position()
        end
        local orig_mouse_get_x = love.mouse.getX
        love.mouse.getX = function()
            if BalatroBridge.cursor_active then
                return BalatroBridge.cursor_x
            end
            return orig_mouse_get_x()
        end
        local orig_mouse_get_y = love.mouse.getY
        love.mouse.getY = function()
            if BalatroBridge.cursor_active then
                return BalatroBridge.cursor_y
            end
            return orig_mouse_get_y()
        end
        local orig_mouse_is_down = love.mouse.isDown
        love.mouse.isDown = function(...)
            if BalatroBridge.cursor_active then
                local args = {...}
                for _, b in ipairs(args) do
                    if BalatroBridge.mouse_down[b] then return true end
                end
                return false
            end
            return orig_mouse_is_down(...)
        end
    end)

    if not ok then
        log("ERROR in init: " .. tostring(err))
    else
        log("BalatroBridge initialized successfully. PID=" .. tostring(ffi.C.GetCurrentProcessId()))
    end
    return ok
end

function BalatroBridge.update(dt)
    if not BalatroBridge.initialized then return end

    if not BalatroBridge.logged_update then
        BalatroBridge.logged_update = true
        log("First BalatroBridge.update() call. G=" .. tostring(G) .. " G.STAGE=" .. tostring(G and G.STAGE))
    end

    local ok, err = pcall(function()
        local now = ffi.C.GetTickCount64()
        
        -- Update heartbeat (offset 24 = balatroHeartbeatMs)
        local pHeartbeat = ffi.cast("uint64_t*", BalatroBridge.base_ptr + OFF_HEADER + 24)
        pHeartbeat[0] = now

        -- Read GTA State flags
        local gta_flags = ffi.cast("uint32_t*", BalatroBridge.base_ptr + OFF_GTA_STATE + 4)[0]
        local gta_active = bit.band(gta_flags, 0x07) ~= 0
        local pGtaHeartbeat = ffi.cast("uint64_t*", BalatroBridge.base_ptr + OFF_HEADER + 16)[0]
        local gta_alive = (now - pGtaHeartbeat) < 3000

        -- Mute Balatro audio when not active in GTA phone
        if love.audio then
            if gta_active then
                if BalatroBridge.is_muted then
                    local target_vol = 1.0
                    if BalatroBridge.saved_volume and BalatroBridge.saved_volume > 0.05 then
                        target_vol = BalatroBridge.saved_volume
                    end
                    love.audio.setVolume(target_vol)
                    BalatroBridge.is_muted = false
                end
            else
                local cur_vol = love.audio.getVolume()
                if cur_vol and cur_vol > 0.05 then
                    BalatroBridge.saved_volume = cur_vol
                end
                love.audio.setVolume(0.0)
                BalatroBridge.is_muted = true
            end
        end
        
        -- Drain Input Ring
        local pHead = ffi.cast("uint64_t*", BalatroBridge.base_ptr + OFF_INPUT_RING + 0)
        local pTail = ffi.cast("uint64_t*", BalatroBridge.base_ptr + OFF_INPUT_RING + 8)
        local head = pHead[0]
        local tail = pTail[0]

        local win_w = (love.graphics and love.graphics.getWidth()) or 1280
        local win_h = (love.graphics and love.graphics.getHeight()) or 720
        if win_w <= 0 then win_w = 1280 end
        if win_h <= 0 then win_h = 720 end

        local events = ffi.cast("struct InputEvent*", BalatroBridge.base_ptr + OFF_INPUT_RING + 0x40)

        while tail < head do
            local idx = tonumber(tail % INPUT_RING_ENTRIES)
            local ev = events[idx]
            local ev_type = ev.type

            if ev_type == 1 then -- Mouse Move
                local is_over = (ev.extra == 1)
                BalatroBridge.cursor_active = is_over
                BalatroBridge.norm_x = ev.x
                BalatroBridge.norm_y = ev.y
                local new_x = ev.x * win_w
                local new_y = ev.y * win_h
                local dx = new_x - BalatroBridge.cursor_x
                local dy = new_y - BalatroBridge.cursor_y
                BalatroBridge.cursor_x = new_x
                BalatroBridge.cursor_y = new_y

                if is_over and love.mousemoved then
                    love.mousemoved(new_x, new_y, dx, dy, false)
                end

            elseif ev_type == 2 then -- Mouse Button
                local btn = ev.code
                local pressed = (ev.extra == 1)
                BalatroBridge.norm_x = ev.x
                BalatroBridge.norm_y = ev.y
                local cur_x = ev.x * win_w
                local cur_y = ev.y * win_h
                BalatroBridge.cursor_active = true
                BalatroBridge.cursor_x = cur_x
                BalatroBridge.cursor_y = cur_y

                if pressed then
                    BalatroBridge.mouse_down[btn] = true
                    if love.mousepressed then
                        love.mousepressed(cur_x, cur_y, btn, false)
                    end
                else
                    BalatroBridge.mouse_down[btn] = false
                    if love.mousereleased then
                        love.mousereleased(cur_x, cur_y, btn)
                    end
                end

            elseif ev_type == 3 then -- Mouse Wheel
                if love.wheelmoved then
                    love.wheelmoved(0, ev.y)
                end

            elseif ev_type == 5 then -- Reset Input
                for b = 1, 3 do
                    if BalatroBridge.mouse_down[b] then
                        BalatroBridge.mouse_down[b] = false
                        if love.mousereleased then
                            love.mousereleased(BalatroBridge.cursor_x, BalatroBridge.cursor_y, b)
                        end
                    end
                end
                BalatroBridge.cursor_active = false
            end

            tail = tail + 1
        end
        pTail[0] = tail

        if BalatroBridge.cursor_active and G and G.CONTROLLER then
            if not (G.CONTROLLER.HID and G.CONTROLLER.HID.mouse) then
                G.CONTROLLER:set_HID_flags('mouse')
            end
        end

        -- Publish Balatro State (seqlock)
        local pState = ffi.cast("uint32_t*", BalatroBridge.base_ptr + OFF_BALATRO_STATE)
        local seq = pState[0]
        pState[0] = seq + 1

        local flags = 0
        if G then
            if G.STAGE and G.STAGES and G.STAGE == G.STAGES.RUN then flags = bit.bor(flags, 1) end
            if G.STATE and G.STATES and G.STATE == G.STATES.SHOP then flags = bit.bor(flags, 2) end
            if G.STATE and G.STATES and G.STATE == G.STATES.SELECTING_HAND then flags = bit.bor(flags, 4) end
            if G.STATE and G.STATES and G.STATE == G.STATES.GAME_OVER then flags = bit.bor(flags, 8) end
        end

        pState[1] = flags
        pState[2] = (G and G.GAME and G.GAME.dollars) or 0
        pState[3] = (G and G.GAME and G.GAME.round_resets and G.GAME.round_resets.ante) or 1
        pState[4] = (G and G.GAME and G.GAME.round) or 1
        pState[5] = (G and G.GAME and G.GAME.current_round and G.GAME.current_round.hands_left) or 4
        pState[6] = (G and G.GAME and G.GAME.current_round and G.GAME.current_round.discards_left) or 3

        local pChips = ffi.cast("uint64_t*", BalatroBridge.base_ptr + OFF_BALATRO_STATE + 32)
        pChips[0] = (G and G.GAME and G.GAME.chips) or 0
        pChips[1] = (G and G.GAME and G.GAME.blind and G.GAME.blind.chips) or 300

        pState[0] = seq + 2
    end)

    if not ok then
        log("ERROR in update: " .. tostring(err))
    end
end

function BalatroBridge.draw()
    if not BalatroBridge.initialized then return end

    local canvas = (G and G.CANVAS)
    if not canvas then return end

    local w = canvas:getWidth()
    local h = canvas:getHeight()

    if not BalatroBridge.logged_draw then
        BalatroBridge.logged_draw = true
        log("First BalatroBridge.draw() call. CANVAS=" .. tostring(canvas) .. " w=" .. tostring(w) .. " h=" .. tostring(h))
    end

    if w <= 0 or h <= 0 or w > MAX_W or h > MAX_H then return end

    -- Check GTA V state: stream at full 60 FPS when GTA is connected/active
    -- Only idle throttle (1 FPS) if GTA is completely closed / not running
    local gta_flags = ffi.cast("uint32_t*", BalatroBridge.base_ptr + OFF_GTA_STATE + 4)[0]
    local gta_active = bit.band(gta_flags, 0x07) ~= 0
    local pGtaHeartbeat = ffi.cast("uint64_t*", BalatroBridge.base_ptr + OFF_HEADER + 16)[0]
    local now = ffi.C.GetTickCount64()
    local gta_alive = (now - pGtaHeartbeat) < 3000

    if not (gta_active or gta_alive) then
        if (now - BalatroBridge.last_capture_time) < 1000 then
            return
        end
    end
    BalatroBridge.last_capture_time = now

    local ok, err = pcall(function()
        -- High-definition 1080p stream resolution (1920x1080, crystal-clear text across phone, large, and fullscreen)
        local stream_w = 1920
        local stream_h = 1080

        if not BalatroBridge.stream_canvas or BalatroBridge.stream_w ~= stream_w or BalatroBridge.stream_h ~= stream_h then
            BalatroBridge.stream_canvas = love.graphics.newCanvas(stream_w, stream_h)
            BalatroBridge.stream_w = stream_w
            BalatroBridge.stream_h = stream_h
        end

        local scale_x = stream_w / w
        local scale_y = stream_h / h

        if canvas.setFilter then
            canvas:setFilter("linear", "linear")
        end

        love.graphics.push("all")
        love.graphics.setCanvas(BalatroBridge.stream_canvas)
        love.graphics.origin()
        love.graphics.setColor(1, 1, 1, 1)
        love.graphics.setShader()
        love.graphics.setBlendMode("replace", "premultiplied")
        love.graphics.draw(canvas, 0, 0, 0, scale_x, scale_y)

        -- Render pixel-perfect in-game pointer cursor directly onto the stream texture
        if BalatroBridge.cursor_active and BalatroBridge.norm_x and BalatroBridge.norm_y then
            local cx = BalatroBridge.norm_x * stream_w
            local cy = BalatroBridge.norm_y * stream_h

            love.graphics.origin()
            love.graphics.setShader()
            love.graphics.setBlendMode("alpha")

            local is_down = BalatroBridge.mouse_down[1] or false
            local scale = stream_h / 720.0

            local pts = {
                0, 0,
                0, 17 * scale,
                4.5 * scale, 13.5 * scale,
                7.5 * scale, 20 * scale,
                10.5 * scale, 18.5 * scale,
                7.5 * scale, 12 * scale,
                12.5 * scale, 12 * scale
            }

            -- Subtle drop shadow
            love.graphics.push()
            love.graphics.translate(cx + 1.5 * scale, cy + 1.5 * scale)
            love.graphics.setColor(0, 0, 0, 0.45)
            love.graphics.polygon("fill", pts)
            love.graphics.pop()

            -- Sharp dark outline
            love.graphics.push()
            love.graphics.translate(cx, cy)
            love.graphics.setColor(0, 0, 0, 1)
            love.graphics.setLineWidth(2.5 * (scale / 1.5))
            love.graphics.setLineJoin("miter")
            love.graphics.polygon("line", pts)

            -- Fill: classic crisp pointer (subtle tint when clicked)
            if is_down then
                love.graphics.setColor(0.9, 0.9, 0.85, 1)
            else
                love.graphics.setColor(1, 1, 1, 1)
            end
            love.graphics.polygon("fill", pts)
            love.graphics.pop()
        end

        love.graphics.pop()

        -- Readback full 1080p stream
        local imgData = BalatroBridge.stream_canvas:newImageData()
        if not imgData then return end

        local src_ptr = imgData:getFFIPointer()
        if src_ptr == nil then return end

        -- Triple buffer slot selection
        local pCtl = ffi.cast("uint32_t*", BalatroBridge.base_ptr + OFF_OVERLAY_CTL)
        local ctl_state = pCtl[0]
        local middle_slot = bit.band(ctl_state, 3)

        local back = BalatroBridge.back_slot
        if back == middle_slot then
            back = (back + 1) % 3
        end

        -- Copy pixel data into back slot (1.3 MB instead of 5.3 MB)
        local dest_offset = OFF_PIXELS + (back * SLOT_BYTES)
        local dest_ptr = BalatroBridge.base_ptr + dest_offset
        local copy_bytes = stream_w * stream_h * 4
        ffi.copy(dest_ptr, src_ptr, copy_bytes)

        -- Write slot header
        BalatroBridge.frame_id = BalatroBridge.frame_id + 1
        local pSlotHdr = ffi.cast("uint32_t*", BalatroBridge.base_ptr + OFF_OVERLAY_SLOTS + (back * 0x40))
        pSlotHdr[0] = stream_w
        pSlotHdr[1] = stream_h
        pSlotHdr[2] = stream_w * 4
        pSlotHdr[3] = 0 -- RGBA top-down
        
        local pFrameId = ffi.cast("uint64_t*", BalatroBridge.base_ptr + OFF_OVERLAY_SLOTS + (back * 0x40) + 16)
        pFrameId[0] = BalatroBridge.frame_id
        pFrameId[1] = ffi.C.GetTickCount64()

        -- Atomic swap using JIT thunk
        local new_state = bit.bor(back, 4)
        local prev_middle = atomic_xchg(pCtl, new_state)
        BalatroBridge.back_slot = bit.band(prev_middle, 3)

        -- Bump frames published
        local pPub = ffi.cast("uint64_t*", BalatroBridge.base_ptr + OFF_OVERLAY_CTL + 8)
        pPub[0] = pPub[0] + 1
    end)

    if not ok then
        log("ERROR in draw: " .. tostring(err))
    end
end

return BalatroBridge
