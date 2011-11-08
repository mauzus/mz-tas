--$D27C -> $D27D	-	Minimum camera Y position
--$D27E -> $D27F  -	Maximum camera Y position
--$D280 -> $D281	-	Minimum camera X position
--$D282 -> $D283	-	Maximum camera X position
--$D2AD			-	Current level
--$D2AE			-	Current Act
local level = 1
local act = 1

local height_multiplier = 0

local last_screenshot_x = 8978978979879879
local screenshot_width = 160 - 32
local screenshot_heigth = 144 - 40
local fixed_cam_y = (screenshot_heigth*height_multiplier)+8
local fixed_cam_x = 0

local cam_lock = 0
local pos_lock = 0

local address = {
	cam_x           = 0xD288,
	cam_y           = 0xD28A,
	invulnerability = 0xD3A1,
	flight_gauge    = 0xD3AF,

	tails           = 0xD500,
	objects         = 0xD500
}
local offset = {
	flags1    = 0x03,
	flags2    = 0x04,
	x         = 0x11,
	y         = 0x14,
	spd_sub_x = 0x16,
	spd_x     = 0x17,
	spd_sub_y = 0x18,
	spd_y     = 0x19,
	scr_x     = 0x1A,
	scr_y     = 0x1C,
	hp        = 0x26,
	size_x    = 0x2c,
	size_y    = 0x2d
}
local value = {
	objects_num     = 24,
	tails_hb_size_x = 20,
	tails_hb_size_y = 13,
	tails_hb_color  = "#FFFFFF",
	inv_color       = "#FFFF00",
	bomb_hb_size    = 16,
	bomb_hb_color   = "#FF0000",
	enemy_hb_size   = 16,
	enemy_hb_color  = "#FFFFFF",
	hb_transparency = 25,
	cam_off_x       = 48,
	cam_off_y       = 24,
	slot_size       = 0x40
}

local fixed_x = memory.readword(address.tails+offset.x)
local fixed_y = memory.readword(address.tails+offset.y)

function GetGlobalValues()
	-- Camera Position
	value.cam_x             = memory.readword(address.cam_x)
	value.cam_y             = memory.readword(address.cam_y)
end

function GetObjectValues(obj_value,slot_n)
		obj_value.slot_offset = slot_n * value.slot_size
		obj_value.obj_id      = memory.readbyte(address.objects+obj_value.slot_offset)
		obj_value.flags2      = memory.readbyte(address.objects+offset.flags2+obj_value.slot_offset)
		obj_value.hp          = string.format("%x",(memory.readbytesigned(address.objects+offset.hp+obj_value.slot_offset)))
		obj_value.x           = memory.readword(address.objects+offset.x+obj_value.slot_offset)
		obj_value.y           = memory.readword(address.objects+offset.y+obj_value.slot_offset)
		obj_value.hb_size_x   = memory.readbyte(address.objects+offset.size_x+obj_value.slot_offset)
		obj_value.hb_size_y   = memory.readbyte(address.objects+offset.size_y+obj_value.slot_offset)
		obj_value.rel_x       = obj_value.x-value.cam_x - value.cam_off_x
		obj_value.rel_y       = obj_value.y-value.cam_y - value.cam_off_y
		obj_value.hb_rel_x    = obj_value.rel_x
		obj_value.hb_rel_y    = obj_value.rel_y
		obj_value.spd_x       = memory.readwordsigned(address.objects+offset.spd_x+obj_value.slot_offset)
		obj_value.spd_y       = memory.readwordsigned(address.objects+offset.spd_y+obj_value.slot_offset)
end


function ObjectInformation()
	local	count = 0
	for slot_n = 0,value.objects_num-1 do
		local obj_value = {}
		local color_to_use
		GetObjectValues(obj_value,slot_n)

		if true then
			color_to_use = value.enemy_hb_color

			-- object hitbox
			gui.box(obj_value.hb_rel_x-obj_value.hb_size_x-1,
			        obj_value.hb_rel_y,
			        obj_value.hb_rel_x+obj_value.hb_size_x,
			        obj_value.hb_rel_y-obj_value.hb_size_y,
			        color_to_use .. value.hb_transparency)

			-- object hp
			gui.text(obj_value.hb_rel_x,obj_value.hb_rel_y,obj_value.obj_id..":"..obj_value.hp)

			-- position axis
			local AXIS_COLOUR           = 0xFFFFFF
			local AXIS_SIZE             = 2
			gui.drawline(obj_value.hb_rel_x, obj_value.hb_rel_y-AXIS_SIZE, obj_value.hb_rel_x, obj_value.hb_rel_y+AXIS_SIZE, AXIS_COLOUR)
			gui.drawline(obj_value.hb_rel_x-AXIS_SIZE, obj_value.hb_rel_y, obj_value.hb_rel_x+AXIS_SIZE, obj_value.hb_rel_y, AXIS_COLOUR)

		end -- if end

	end -- for end
end

gui.register(function()
	GetGlobalValues()
	ObjectInformation()

	local x_coord = 0
	for i = 0,40 do
		x_coord = (screenshot_width*i) - value.cam_x + 32
		if i==0 then x_coord = 0 end
		gui.line(x_coord,
		         0,
		         x_coord,
		         144,
		         "red")
	end

	local displ = 5
	for i = 0,40 do
		gui.line(0,
		         (40+32+screenshot_heigth*i) - value.cam_y - value.cam_off_y,
		         160,
		         (40+32+screenshot_heigth*i) - value.cam_y - value.cam_off_y,
		        "red")
		for j = 0,16 do
			x_coord = (48+screenshot_width*j) - value.cam_x - value.cam_off_x + displ + 32
			if j==0 then x_coord = x_coord - 32 end
			gui.text(
			x_coord,
			(40+32+screenshot_heigth*i) - value.cam_y - value.cam_off_y + displ,
			i..":"..j)
		end
	end

	local gui_x, gui_y = 40,2
	local cam_lock_str = ""
	local pos_lock_str = ""
	if cam_lock ~= 0 then cam_lock_str = "*" end
	if pos_lock ~= 0 then pos_lock_str = "+" end
	gui.text(gui_x, gui_y+ 0, "lvl: "..  level.."-"..act)
	gui.text(gui_x, gui_y+ 8, "pos: "..  fixed_x..":"..fixed_y)
	gui.text(gui_x, gui_y+16, "cam: "..  value.cam_x..":"..value.cam_y-8)
	gui.text(gui_x, gui_y+24, "hgt: "..  height_multiplier .." "..cam_lock_str..pos_lock_str)
end)

local level_pressed    = 0
local act_pressed      = 0
local cam_lock_pressed = 0
local pos_lock_pressed = 0

emu.registerafter( function()
	GetGlobalValues()

	-- manual tails movement
	local keys = input.get()
	local manual_spd = 1
	if keys.J then
		fixed_x = fixed_x-manual_spd
	end
	if keys.L then
		fixed_x = fixed_x+manual_spd
	end
	if keys.I then
		fixed_y = fixed_y-manual_spd
	end
	if keys.K then
		fixed_y = fixed_y+manual_spd
	end

	if (((value.cam_x%screenshot_width) == 0) and (last_screenshot_x ~= value.cam_x) and (cam_lock ~= 0)) then
		last_screenshot_x = value.cam_x
		gui.savescreenshot()
	end

	if cam_lock == 0 then
		height_multiplier = math.floor(math.abs((fixed_y-72) / 104))
	end
--	if keys.X and height_pressed == 0 then
--		height_multiplier = height_multiplier + 1
--		fixed_cam_y = (screenshot_heigth*height_multiplier)+8
--		height_pressed = 1
--	end
--	if keys.Z and height_pressed == 0 then
--		height_multiplier = height_multiplier - 1
--		fixed_cam_y = (screenshot_heigth*height_multiplier)+8
--		height_pressed = 1
--	end
--	if ((not keys.X) and (not keys.Z)) then
--		height_pressed = 0
--	end

	if keys.H and level_pressed == 0 then
		level = level + 1
		level_pressed = 1
	end
	if keys.F and level_pressed == 0 then
		level = level - 1
		level_pressed = 1
	end
	if ((not keys.H) and (not keys.F)) then level_pressed = 0 end
	if keys.T and act_pressed == 0 then
		act = act + 1
		act_pressed = 1
	end
	if keys.G and act_pressed == 0 then
		act = act - 1
		act_pressed = 1
	end
	if ((not keys.T) and (not keys.G)) then act_pressed = 0 end

	if keys.C and cam_lock_pressed == 0 then
		if cam_lock ~= 0 then cam_lock = 0
		else
			cam_lock = 1
			height_multiplier = math.floor(math.abs((fixed_y-72) / 104))
			fixed_cam_y = (screenshot_heigth*height_multiplier)+8
		end
		cam_lock_pressed = 1
	end
	if (not keys.C) then
		cam_lock_pressed = 0
	end
	if keys.X and pos_lock_pressed == 0 then
		if pos_lock ~= 0 then pos_lock = 0
		else
			pos_lock = 1
		end
		pos_lock_pressed = 1
	end
	if (not keys.X) then
		pos_lock_pressed = 0
	end
	if pos_lock == 0 then
		fixed_x = memory.readword(address.tails+offset.x)
		fixed_y = memory.readword(address.tails+offset.y)
	end

	if keys.M then
		os.execute('del /Q shots')
	end

	-- cheats
	memory.writeword(address.tails+offset.hp,0x20)
	memory.writebyte(0xd3a1, 0xff)
	if pos_lock ~= 0 then memory.writeword(0xd511, fixed_x) end
	if pos_lock ~= 0 then memory.writeword(0xd514, fixed_y) end
	if cam_lock ~= 0 then memory.writeword(address.cam_y, fixed_cam_y) end
	memory.writebyte(0xd506, 0)
end)

--memory.registerwrite(address.cam_x, 2, function()
--	memory.writeword(address.cam_x, fixed_cam_x)
--end)

memory.registerwrite(address.cam_y, 2, function()
	if cam_lock ~= 0 then memory.writeword(address.cam_y, fixed_cam_y) end
end)

memory.registerwrite(address.tails+offset.x, 2, function()
	if pos_lock ~= 0 then memory.writeword(address.tails+offset.x, fixed_x) end
end)

memory.registerwrite(address.tails+offset.y, 2, function()
	if pos_lock ~= 0 then memory.writeword(address.tails+offset.y, fixed_y) end
end)

-- animation
memory.registerwrite(0xD506, 1, function()
	memory.writebyte(0xD506, 0)
end)

memory.registerwrite(0xD2AD, 1, function()
	memory.writebyte(0xD2AD, level)
end)

memory.registerwrite(0xD2AE, 1, function()
	memory.writebyte(0xD2AE, act)
end)

while true do
	memory.writebyte(0xD2AD, level)
	memory.writebyte(0xD2AE, act)
	emu.frameadvance()
end
