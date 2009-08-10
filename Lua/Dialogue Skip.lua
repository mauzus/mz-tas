-- press the X button at the first possible
-- frame after each box of dialogue

mem1 = 0x83054
mem2 = 0x83018
mem3 = 0x83014
mem1Pressed = 0
mem2Pressed = 0
mem3Pressed = 0

while (true) do
	if memory.readbyte(mem1) == 1 then
		if mem1Pressed == 0 then
			joypad.set(1,{["x"]=true})
			mem1Pressed = 1
		end
	else
		mem1Pressed = 0
	end

	if memory.readbyte(mem2) == 1 then
		if mem2Pressed == 0 then
			joypad.set(1,{["x"]=true})
			mem2Pressed = 1
		end
	else
		mem2Pressed = 0
	end

	if memory.readbyte(mem3) == 10 then
		if mem3Pressed == 0 then
			joypad.set(1,{["x"]=true})
			mem3Pressed = 1
		end
	else
		mem3Pressed = 0
	end

	emu.frameadvance()
end
