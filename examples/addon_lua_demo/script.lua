-- Paste into the script.lua of any addon (in-game Addon Editor). The addon still
-- works without StormFabric: `sf_demo` is simply nil when the mod is not loaded.

function onCreate(is_world_create)
    if sf_demo then
        local reply, count = sf_demo.ping()
        sf_demo.log("onCreate: ping -> " .. tostring(reply) .. " #" .. tostring(count))
    end
end

-- Type ?sf in the chat.
function onCustomCommand(full_message, user_peer_id, is_admin, is_auth, command)
    if command ~= "?sf" then return end
    if not sf_demo then
        server.announce("StormFabric", "sf_demo is not available (mod disabled or unsupported build)", user_peer_id)
        return
    end
    local reply, count = sf_demo.ping()
    server.announce("StormFabric", reply .. " #" .. tostring(count), user_peer_id)
    -- The two calls below are made by the C++ mod itself through the game's API.
    if sf_demo.announce then
        local ok = sf_demo.announce("this line was sent by C++ calling server.announce")
        local name = sf_demo.hostName()
        sf_demo.log("?sf: announce=" .. tostring(ok) .. " hostName=" .. tostring(name))
        server.announce("StormFabric", "C++ read host name: " .. tostring(name), user_peer_id)
    end
end
