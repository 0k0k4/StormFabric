-- Enable p0_demo BEFORE spawning/reloading this microcontroller: libraries are
-- installed only in newly created VMs. Connect video output to a powered monitor.
local status = "Waiting for P0 SDK"
local ran = false
function onTick()
    if ran then return end
    ran = true
    if not p0_test or not p0_test.run then
        status = "Reload vehicle after enabling p0_demo"
        return
    end
    local ok, message = p0_test.run()
    status = message or (ok and "PASS" or "FAIL (see log)")
    output.setBool(1, ok == true)
end
function onDraw()
    screen.setColor(255, 255, 255)
    screen.drawTextBox(1, 1, screen.getWidth()-2, screen.getHeight()-2, status, 0, 0)
end
