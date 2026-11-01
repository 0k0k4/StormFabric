-- Connect camera -> this Lua block -> powered vehicle monitor.
-- Enable video_fx BEFORE spawning the vehicle (new microcontroller VM required).
function onDraw()
    local w, h = screen.getWidth(), screen.getHeight()
    video.setOffset(4, 0)
    video.setScale(1.25)
    video.setTint(160, 255, 160)
    video.setFilter(0.5, 0) -- grayscale amount, invert amount (0..1)

    -- Original source pixel BEFORE effects and screen overlay, not the final 3D image.
    local r, g, b, a, ready, sequence = video.getPixel(math.floor(w/2), math.floor(h/2))
    screen.setColor(255, 255, 255)
    screen.drawText(1, 1, ready and ("RGB " .. r .. "," .. g .. "," .. b) or "Capture pending")
    screen.drawText(1, 8, "Frame " .. sequence)
end
