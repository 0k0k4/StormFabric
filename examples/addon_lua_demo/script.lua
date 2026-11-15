-- Paste into the script.lua of any addon (in-game Addon Editor). The addon still
-- works without StormFabric: `sf_demo` is simply nil when the mod is not loaded.

function onCreate(is_world_create)
    if sf_demo then
        local reply, count = sf_demo.ping()
        sf_demo.log("onCreate: ping -> " .. tostring(reply) .. " #" .. tostring(count))
        server.announce("StormFabric", "native library answered: " .. tostring(reply))
    else
        server.announce("StormFabric", "sf_demo is not available (mod disabled or unsupported build)")
    end
end

function onCustomCommand(full_message, user_peer_id, is_admin, is_auth, command)
    if command == "?sf" and sf_demo then
        local reply, count = sf_demo.ping()
        server.announce("StormFabric", reply .. " #" .. tostring(count), user_peer_id)
    end
end
