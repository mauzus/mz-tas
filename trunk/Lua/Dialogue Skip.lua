-- press the X button at the first possible
-- frame after each box of dialogue

pReady = { 0x83014, 0x83018, 0x83054, 0x830CC, 0x8308C, 0x83090 }
btPressed = {    0,       0,       0,       0,       0,       0 }

while (true) do

	for i = 1,# btPressed do
		ready = memory.readbyte(pReady[i])
		if ready == 1 then
			if btPressed[i] == 0 or btPressed[i] == 10 then
				joypad.set(1,{["x"]=true})
				btPressed[i] = 1
			end
		elseif ready == 10 then
			if btPressed[i] == 0 or btPressed[i] == 1 then
				joypad.set(1,{["x"]=true})
				btPressed[i] = 10
			end
		else
			btPressed[i] = 0
		end
--		gui.text(40,10+(i*8),btPressed[i] .. " " .. ready)
	end

--	mem  = 0x83088
--	for i=0,32 do
--		gui.text(40,10+(i*8),mem+(i*4) .. " " .. memory.readbyte(mem+(i*4)))
--	end

	emu.frameadvance()
end
