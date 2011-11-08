<?php
$map              = "4-3";
//$debug_lines      = 1;
$print_in_browser = 1;

$osd_width    = 32; $osd_height   = 40;
require('map_info.php');
if (!isset($map_info[$map]))
	exit("<b>ERROR:</b> There's no info for stage <b>".$map."</b>.");
$level_height = $map_info[$map]['level_height'];
$level_width  = $map_info[$map]['level_width'];
$height_crop  = $map_info[$map]['height_crop'];
$width_extra  = $map_info[$map]['width_extra'];

//$level_height = 4;
//$level_width  = 15;
//
//$height_crop  = 0;
//$height_crop  = 17; // horizontal levels
////$height_crop  = 41; // vertical levels
//$width_extra  = 160 - $osd_width;
//$width_extra  = $osd_width;

$screen_width = 160 - $osd_width;
$screen_height = 144 - $osd_height;
$shots_dir = "shots/" . $map . "/";
$shots_dir_new = "temp/";

for ($j = 0; $j <= $level_height; $j++) {
	for ($i = 0; $i <= $level_width; $i++) {
		$src_x = $osd_width;
		$src_y = $osd_height;
		$img_width  = $screen_width;
		$img_height = $screen_height;
		if ($i == 0) { $src_x = 0; $img_width  += $osd_width; }
		if ($j == 0) { $src_y = 0; $img_height += $osd_height; }
		cut_image($j, $i, $src_x, $src_y, $img_width, $img_height);
	}
}

create_map($map, $level_width+1,$level_height+1);

foreach (glob($shots_dir_new."*.png") as $filename) {
	unlink($filename);
}

function cut_image($y, $x, $src_x, $src_y, $screen_width, $screen_height) {
	global $shots_dir, $shots_dir_new;

	$dega_shots_dir = "E:/MinGW/projects/dega/shots/";
	$filename = str_pad($y, 3, "0", STR_PAD_LEFT) . "_" . str_pad($x, 3, "0", STR_PAD_LEFT) . ".png";
	$dega_filename = $dega_shots_dir."Tails Adventures (JU) [!]_".str_pad($x, 3, "0", STR_PAD_LEFT).".png";

	if (!file_exists($shots_dir))
		mkdir($shots_dir);
	if (file_exists($dega_filename) && !file_exists($shots_dir.$filename)) {
//		if (file_exists($shots_dir.$filename))
//			unlink($shots_dir.$filename);
		rename($dega_filename, $shots_dir.$filename);
	}
	if (!file_exists($shots_dir.$filename)) {
//		echo '<h2>"'.$shots_dir.$filename.'" doesn\'t exist! Image was not created.</h2>';
//		return;
		$img = imagecreatetruecolor(160, 144);
		imagefilledrectangle($img, 0, 0, 160, 144, imagecolorallocate($img, 0, 255, 0));
	}
	else {
		$img     = imagecreatefrompng($shots_dir.$filename);
		if ($img === false) {
			echo '<h2>"'.$shots_dir.$filename.'" is damaged! Image was not created.</h2>';
			return;
		}
	}

	// create a new temporary image
	$tmp_img = imagecreatetruecolor($screen_width, $screen_height);

	// copy and resize old image into new image 
	imagecopy($tmp_img, $img, 0, 0, $src_x, $src_y, $screen_width, $screen_height);

	// save image into a file
	imagepng($tmp_img, $shots_dir_new.$filename);

	// print image directly
//header('Content-type: image/png');
//imagepng($tmp_img, null);
}

function create_map($map_filename, $size_x, $size_y) {
	global $osd_width, $osd_height, $screen_width, $screen_height, $shots_dir_new, $height_crop, $width_extra;
	global $debug_lines, $print_in_browser;

	// create a new temporary image
	$img_width = ($size_x-1)*$screen_width+$width_extra+$osd_width;
	$img_height = $size_y*$screen_height-$osd_height-$height_crop;
	$tmp_img = imagecreatetruecolor($img_width, $img_height);
	imagefilledrectangle($tmp_img, 0, 0, $img_width, $img_height, imagecolorallocate($tmp_img, 255, 0, 255));
	$line_color = imagecolorallocate($tmp_img, 255, 0, 0);

	$dest_y = 0;
	for ($j = 0; $j < $size_y; $j++) {
		$dest_x = 0;
		for ($i = 0; $i < $size_x; $i++) {
			$filename = str_pad($j, 3, "0", STR_PAD_LEFT) . "_" . str_pad($i, 3, "0", STR_PAD_LEFT) . ".png";
			if (!file_exists($shots_dir_new.$filename)) {
				echo '<h2>"'.$shots_dir_new.$filename.'" doesn\'t exist!! Image was not created.</h2>';
				return;
			}
			$img     = imagecreatefrompng($shots_dir_new.$filename);
			if ($img === false) {
				echo '<h2>"'.$shots_dir_new.$filename.'" is damaged!! Image was not created.</h2>';
				return;
			}
			// copy and resize old image into new image
			$src_width  = $screen_width;
			$src_height = $screen_height;
			if ($i == 0) $src_width  += $osd_width;
			if ($j == 0) $src_height += $osd_height;
			if ($i == ($size_x-1)) $dest_x = $img_width-$screen_width;
			imagecopy($tmp_img, $img, $dest_x, $dest_y, 0, 0, $src_width, $src_height);
			if (isset($debug_lines) && $debug_lines) {
				imageline($tmp_img, $dest_x, 0, $dest_x, $img_height, $line_color);
				imagestring($tmp_img, 1, $dest_x+5, $dest_y+5, $j.":".$i, $line_color);
			}
			$dest_x += $src_width;
		}
		if (isset($debug_lines) && $debug_lines) {
			imageline($tmp_img, 0, $dest_y, $img_width, $dest_y, $line_color);
		}
		$dest_y += $src_height;
	}
	// save image into a file
	imagepng($tmp_img, $map_filename.".png");

	// print image directly
	if (isset($print_in_browser) && $print_in_browser) {
		if (file_exists($map_filename.".png"))
			echo '<img src="'.$map_filename.'.png" />';
	}
}
