from __future__ import annotations

import argparse
import concurrent.futures
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
from dataclasses import dataclass
from html import escape
from pathlib import Path
from typing import Any
from xml.etree import ElementTree as ET

try:
	import cairosvg
except Exception:  # pragma: no cover - exercised in user env
	cairosvg = None

try:
	from PIL import Image
except ImportError as exc:  # pragma: no cover - exercised in user env
	interpreter = Path(sys.executable).resolve() if sys.executable else "python"
	sys.stderr.write(
		"face_codegen could not import Pillow in the active interpreter.\n"
		f"Interpreter: {interpreter}\n"
		"Install the dependencies into that exact interpreter with:\n"
		f"  \"{interpreter}\" -m pip install --user cairosvg Pillow\n"
	)
	raise SystemExit(2) from exc


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_INPUT = REPO_ROOT / "assets" / "face" / "kerfur_faces.json"
DEFAULT_OUT_INCLUDE = REPO_ROOT / "include" / "ui" / "generated"
DEFAULT_OUT_SRC = REPO_ROOT / "src" / "ui" / "generated"
MAX_EFFECTS = 6
CANVAS_WIDTH = 128
WHISKER_LEFT_X = 0
WHISKER_Y = 46

CHROME_CANDIDATES = [
	Path("C:/Program Files/Google/Chrome/Application/chrome.exe"),
	Path("C:/Program Files (x86)/Google/Chrome/Application/chrome.exe"),
	Path("C:/Program Files/Chromium/Application/chrome.exe"),
]


def c_ident(value: str) -> str:
	text = re.sub(r"[^0-9A-Za-z]+", "_", value).strip("_")
	text = re.sub(r"_+", "_", text)
	if not text:
		text = "unnamed"
	if text[0].isdigit():
		text = f"_{text}"
	return text.upper()


def make_enum(prefix: str, value: str) -> str:
	return f"{prefix}_{c_ident(value)}"


def make_symbol(prefix: str, value: str) -> str:
	return f"{prefix}_{c_ident(value).lower()}"


def read_json(path: Path) -> dict[str, Any]:
	return json.loads(path.read_text(encoding="utf-8"))


def parse_enum_names(header_path: Path, enum_name: str, prefix: str) -> list[str]:
	text = header_path.read_text(encoding="utf-8")
	match = re.search(rf"enum\s+{re.escape(enum_name)}\s*\{{(.*?)\}};", text, re.S)
	if match is None:
		raise ValueError(f"Could not find enum {enum_name} in {header_path}")

	names: list[str] = []
	for raw_line in match.group(1).splitlines():
		line = raw_line.split("/*", 1)[0].split("//", 1)[0].strip()
		if not line:
			continue
		line = line.rstrip(",")
		if "=" in line:
			line = line.split("=", 1)[0].strip()
		if line.startswith(prefix):
			names.append(line)
	return names


def merge_override(base: dict[str, Any] | None, extra: dict[str, Any] | None) -> dict[str, int]:
	merged: dict[str, int] = {}
	if base and "x" in base:
		merged["x"] = int(base["x"])
	if base and "y" in base:
		merged["y"] = int(base["y"])
	if extra and "x" in extra:
		merged["x"] = int(extra["x"])
	if extra and "y" in extra:
		merged["y"] = int(extra["y"])

	merged["dx"] = int(base.get("dx", 0) if base else 0) + int(extra.get("dx", 0) if extra else 0)
	merged["dy"] = int(base.get("dy", 0) if base else 0) + int(extra.get("dy", 0) if extra else 0)
	return merged


def apply_override(point: tuple[int, int], override: dict[str, int] | None) -> tuple[int, int]:
	x, y = point
	if override:
		if "x" in override:
			x = int(override["x"])
		if "y" in override:
			y = int(override["y"])
		x += int(override.get("dx", 0))
		y += int(override.get("dy", 0))
	return x, y


def format_c_string(text: str) -> str:
	return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


PUPIL_SWAP_MAP = {
	"instant": 0,
	"on_blink": 1,
	"settle": 2,
}


def format_c_array(values: list[int], indent: str = "\t") -> str:
	if not values:
		return "{}"

	chunks: list[str] = []
	for index in range(0, len(values), 12):
		chunk = ", ".join(f"0x{value:02x}" for value in values[index:index + 12])
		chunks.append(f"{indent}{chunk}")
	return "{\n" + ",\n".join(chunks) + "\n}"


def write_if_changed(path: Path, content: str) -> bool:
	if path.exists() and path.read_text(encoding="utf-8") == content:
		return False
	path.parent.mkdir(parents=True, exist_ok=True)
	path.write_text(content, encoding="utf-8")
	return True


def find_chrome_binary() -> Path | None:
	for candidate in CHROME_CANDIDATES:
		if candidate.exists():
			return candidate

	which = shutil.which("chrome.exe") or shutil.which("chrome")
	if which:
		return Path(which)
	return None


def bitmap_from_image(image: Image.Image, draw_black: bool) -> AssetBitmap:
	rgba = image.convert("RGBA")
	width, height = rgba.size
	stride = (width + 7) // 8
	packed = [0] * (stride * height)

	for y in range(height):
		for x in range(width):
			r, g, b, alpha = rgba.getpixel((x, y))
			luma = (r * 299 + g * 587 + b * 114) // 1000
			is_on = False
			if alpha >= 128:
				if draw_black:
					is_on = luma <= 200
				else:
					is_on = luma >= 56
			if is_on:
				index = (y * stride) + (x // 8)
				packed[index] |= 1 << (7 - (x % 8))

	return AssetBitmap(width=width, height=height, stride=stride, data=packed)


@dataclass(frozen=True)
class AssetBitmap:
	width: int
	height: int
	stride: int
	data: list[int]


@dataclass(frozen=True)
class AssetRef:
	group: str
	asset_id: str


def _parse_float(value: str | None, default: float = 0.0) -> float:
	if value is None or value == "":
		return default
	return float(value)


def _transform_points(
	points: list[tuple[float, float]],
	transform: str | None,
) -> list[tuple[float, float]]:
	if not transform:
		return points

	transform = transform.strip()
	match = re.fullmatch(
		r"rotate\(\s*(-?\d+(?:\.\d+)?)\s+(-?\d+(?:\.\d+)?)\s+(-?\d+(?:\.\d+)?)\s*\)",
		transform,
	)
	if match is None:
		raise ValueError(f"Unsupported SVG transform {transform!r}")

	angle = float(match.group(1))
	origin_x = float(match.group(2))
	origin_y = float(match.group(3))
	angle_norm = angle % 360.0

	def rotate_point(px: float, py: float) -> tuple[float, float]:
		dx = px - origin_x
		dy = py - origin_y
		if angle_norm == 0.0:
			return px, py
		if angle_norm == 90.0:
			return origin_x - dy, origin_y + dx
		if angle_norm == 180.0:
			return origin_x - dx, origin_y - dy
		if angle_norm == 270.0:
			return origin_x + dy, origin_y - dx
		raise ValueError(f"Unsupported SVG rotation angle {angle!r}")

	return [rotate_point(px, py) for px, py in points]


def _parse_svg_path_polygons(path_data: str) -> list[list[tuple[float, float]]]:
	tokens = re.findall(r"[MLHVZ]|-?\d+(?:\.\d+)?", path_data)
	polygons: list[list[tuple[float, float]]] = []
	points: list[tuple[float, float]] = []
	index = 0
	current_x = 0.0
	current_y = 0.0
	start_x = 0.0
	start_y = 0.0

	while index < len(tokens):
		token = tokens[index]
		index += 1
		if token == "M":
			if points:
				polygons.append(points)
			current_x = float(tokens[index])
			current_y = float(tokens[index + 1])
			index += 2
			start_x = current_x
			start_y = current_y
			points = [(current_x, current_y)]
		elif token == "L":
			current_x = float(tokens[index])
			current_y = float(tokens[index + 1])
			index += 2
			points.append((current_x, current_y))
		elif token == "H":
			current_x = float(tokens[index])
			index += 1
			points.append((current_x, current_y))
		elif token == "V":
			current_y = float(tokens[index])
			index += 1
			points.append((current_x, current_y))
		elif token == "Z":
			if points and points[0] != points[-1]:
				points.append((start_x, start_y))
			if points:
				polygons.append(points)
				points = []
		else:
			raise ValueError(f"Unsupported SVG path token {token!r}")

	if points:
		if points[0] != points[-1]:
			points.append(points[0])
		polygons.append(points)

	return polygons


def _point_in_polygon(x: float, y: float, polygon: list[tuple[float, float]]) -> bool:
	inside = False
	for index in range(len(polygon) - 1):
		x1, y1 = polygon[index]
		x2, y2 = polygon[index + 1]
		if ((y1 > y) != (y2 > y)) and (y2 != y1):
			x_cross = ((x2 - x1) * (y - y1) / (y2 - y1)) + x1
			if x < x_cross:
				inside = not inside
	return inside


def _bitmap_from_svg_primitives(path: Path, width: int, height: int) -> AssetBitmap:
	tree = ET.parse(path)
	root = tree.getroot()
	view_box = root.attrib.get("viewBox")
	if view_box:
		parts = [float(part) for part in view_box.replace(",", " ").split()]
		if len(parts) != 4:
			raise ValueError(f"Unsupported viewBox in {path}: {view_box}")
		view_min_x, view_min_y, view_width, view_height = parts
	else:
		view_min_x = 0.0
		view_min_y = 0.0
		view_width = _parse_float(root.attrib.get("width"), float(width))
		view_height = _parse_float(root.attrib.get("height"), float(height))

	# Each SVG element becomes its own polygon group. Sub-paths inside a
	# single <path> are combined with the even-odd fill rule so that inner
	# contours (mouth cavities, pupil highlights) cancel the enclosing
	# region and become transparent holes. Separate top-level elements are
	# OR-ed together so that overlapping rects in e.g. eye_white_blink_crying
	# still form a union rather than carving holes in each other.
	polygon_groups: list[list[list[tuple[float, float]]]] = []

	for element in root:
		tag = element.tag.rsplit("}", 1)[-1]
		if tag == "rect":
			x = _parse_float(element.attrib.get("x"), 0.0)
			y = _parse_float(element.attrib.get("y"), 0.0)
			rect_width = _parse_float(element.attrib.get("width"), 0.0)
			rect_height = _parse_float(element.attrib.get("height"), 0.0)
			rect_points = [
				(x, y),
				(x + rect_width, y),
				(x + rect_width, y + rect_height),
				(x, y + rect_height),
				(x, y),
			]
			polygon_groups.append([
				_transform_points(rect_points, element.attrib.get("transform")),
			])
		elif tag == "path":
			path_data = element.attrib.get("d", "")
			polygon_groups.append(_parse_svg_path_polygons(path_data))
		else:
			raise ValueError(f"Unsupported SVG element <{tag}> in {path}")

	stride = (width + 7) // 8
	packed = [0] * (stride * height)

	for py in range(height):
		for px in range(width):
			user_x = view_min_x + ((px + 0.5) * view_width / width)
			user_y = view_min_y + ((py + 0.5) * view_height / height)
			is_on = False

			for group in polygon_groups:
				group_inside = False
				for polygon in group:
					if _point_in_polygon(user_x, user_y, polygon):
						group_inside = not group_inside
				if group_inside:
					is_on = True
					break

			if is_on:
				index = (py * stride) + (px // 8)
				packed[index] |= 1 << (7 - (px % 8))

	return AssetBitmap(width=width, height=height, stride=stride, data=packed)


def _render_svg_with_chrome(path: Path, width: int, height: int) -> Image.Image:
	chrome = find_chrome_binary()
	if chrome is None:
		raise RuntimeError(
			"SVG rasterization requires cairosvg with a working Cairo runtime, "
			"or a local Chrome/Chromium binary for the headless fallback."
		)

	html = (
		"<!doctype html><html><head><meta charset='utf-8'></head>"
		"<body style='margin:0;background:transparent'>"
		f"<img src='{escape(path.resolve().as_uri(), quote=True)}' "
		f"width='{width}' height='{height}' style='display:block'>"
		"</body></html>"
	)

	temp_root = REPO_ROOT / ".tmp" / f"face_codegen_{os.getpid()}_{threading.get_ident()}"
	if temp_root.exists():
		shutil.rmtree(temp_root, ignore_errors=True)
	temp_root.mkdir(parents=True, exist_ok=True)
	html_path = temp_root / "render.html"
	png_path = temp_root / "render.png"
	profile_path = temp_root / "chrome-profile"
	profile_path.mkdir(parents=True, exist_ok=True)

	try:
		html_path.write_text(html, encoding="utf-8")
		command = [
			str(chrome),
			"--headless",
			"--disable-gpu",
			"--disable-crash-reporter",
			"--disable-breakpad",
			"--no-first-run",
			"--no-default-browser-check",
			"--hide-scrollbars",
			"--default-background-color=00000000",
			"--force-device-scale-factor=1",
			f"--user-data-dir={profile_path}",
			f"--window-size={width},{height}",
			f"--screenshot={png_path}",
			str(html_path),
		]
		result = subprocess.run(command, capture_output=True, text=True, check=False)
		if result.returncode != 0 or not png_path.exists():
			raise RuntimeError(
				f"Chrome SVG rasterization failed for {path}: {result.stderr.strip() or result.stdout.strip()}"
			)
		return Image.open(png_path).convert("RGBA")
	finally:
		shutil.rmtree(temp_root, ignore_errors=True)


def rasterize_svg_to_bitmap(path: Path, width: int, height: int, draw_black: bool) -> AssetBitmap:
	try:
		return _bitmap_from_svg_primitives(path, width, height)
	except ValueError:
		pass

	if cairosvg is not None:
		try:
			png_bytes = cairosvg.svg2png(
				url=str(path),
				output_width=width,
				output_height=height,
				unsafe=True,
			)
			image = Image.open(io.BytesIO(png_bytes))
			return bitmap_from_image(image, draw_black=draw_black)
		except OSError:
			pass

	image = _render_svg_with_chrome(path, width, height)
	return bitmap_from_image(image, draw_black=draw_black)


class FaceCodegen:
	def __init__(self, json_path: Path) -> None:
		self.json_path = json_path
		self.data = read_json(json_path)
		self.asset_groups: dict[str, dict[str, Any]] = self.data["asset_groups"]
		self.assets_by_group: dict[str, list[dict[str, Any]]] = {
			name: list(group.get("items", [])) for name, group in self.asset_groups.items()
		}
		self.asset_lookup: dict[str, AssetRef] = {}
		self.asset_records: list[dict[str, Any]] = []
		self.warnings: list[str] = []
		self._collect_assets()

		self.pet_expressions = parse_enum_names(
			REPO_ROOT / "include" / "behavior" / "pet_state.h",
			"pet_expression",
			"PET_EXPR_",
		)
		self.micro_reactions = parse_enum_names(
			REPO_ROOT / "include" / "behavior" / "micro_reaction.h",
			"micro_reaction_type",
			"REACTION_",
		)

		self.defaults = self.data["defaults"]
		self.anchors = self.defaults.get("anchors", {})
		self.canvas = self.defaults.get("canvas", {})
		self.canvas_width = int(self.canvas.get("width", CANVAS_WIDTH))
		self.expression_lookup = {item["id"]: item for item in self.data.get("expressions", [])}
		self.reaction_lookup = {item["id"]: item for item in self.data.get("reactions", [])}
		self.micro_animation_lookup = {item["id"]: item for item in self.data.get("micro_animations", [])}

	def _collect_assets(self) -> None:
		for group_name, items in self.assets_by_group.items():
			for item in items:
				asset_id = item.get("id")
				if not asset_id:
					raise ValueError(f"Asset without id in group {group_name}")
				if asset_id in self.asset_lookup:
					prev = self.asset_lookup[asset_id]
					raise ValueError(
						f"Duplicate asset id {asset_id!r} in groups {prev.group} and {group_name}"
					)
				self.asset_lookup[asset_id] = AssetRef(group=group_name, asset_id=asset_id)
				self.asset_records.append({"group": group_name, "item": item})

	def _asset_item(self, asset_id: str | None) -> dict[str, Any] | None:
		if not asset_id:
			return None
		ref = self.asset_lookup.get(asset_id)
		if ref is None:
			return None
		for item in self.assets_by_group[ref.group]:
			if item["id"] == asset_id:
				return item
		return None

	def _effect_id_from_entry(self, entry: Any) -> str | None:
		if isinstance(entry, str):
			return entry
		if isinstance(entry, dict):
			return entry.get("effect_id") or entry.get("id")
		return None

	def _require_asset(self, asset_id: str, expected_group: str, context: str) -> None:
		ref = self.asset_lookup.get(asset_id)
		if ref is None:
			raise ValueError(f"Unknown asset {asset_id!r} referenced by {context}")
		if ref.group != expected_group:
			raise ValueError(
				f"Asset {asset_id!r} referenced by {context} is in group {ref.group}, expected {expected_group}"
			)

	def validate(self) -> None:
		expression_ids = [item["id"] for item in self.data.get("expressions", [])]
		reaction_ids = [item["id"] for item in self.data.get("reactions", [])]

		if expression_ids != self.pet_expressions:
			raise ValueError(
				"Expression ids in kerfur_faces.json must exactly match enum pet_expression order. "
				f"json={expression_ids}, code={self.pet_expressions}"
			)
		if reaction_ids != self.micro_reactions[:-1]:
			raise ValueError(
				"Reaction ids in kerfur_faces.json must exactly match enum micro_reaction_type order. "
				f"json={reaction_ids}, code={self.micro_reactions[:-1]}"
			)

		referenced_assets: set[str] = set()

		for anim in self.data.get("micro_animations", []):
			anim_id = anim["id"]
			anim_type = anim.get("type", "")
			if anim_type not in {"look_target", "pupil_motion", "effect_spawn"}:
				raise ValueError(f"Unsupported micro animation type {anim_type!r} for {anim_id}")
			effect_id = anim.get("params", {}).get("effect_id")
			if effect_id:
				self._require_asset(effect_id, "effects", f"micro_animation {anim_id}")
				referenced_assets.add(effect_id)

		for expr in self.data.get("expressions", []):
			expr_id = expr["id"]
			for key, group in (
				("left_eye_white", "eye_whites"),
				("right_eye_white", "eye_whites"),
				("left_eyeball", "pupils"),
				("right_eyeball", "pupils"),
				("left_brow", "eyebrows"),
				("right_brow", "eyebrows"),
				("mouth", "mouths"),
				("whiskers", "whiskers"),
				("indicator", "indicators"),
				("overlay", "overlays"),
				("default_indicator", "indicators"),
				("default_overlay", "overlays"),
				("blink_left_eye_white", "eye_whites"),
				("blink_right_eye_white", "eye_whites"),
			):
				value = expr.get(key)
				if value:
					self._require_asset(value, group, f"expression {expr_id}.{key}")
					referenced_assets.add(value)

			for effect_id in expr.get("default_effects", []):
				self._require_asset(effect_id, "effects", f"expression {expr_id}.default_effects")
				referenced_assets.add(effect_id)

			for motion_id in expr.get("ambient_motion", []):
				if motion_id not in self.micro_animation_lookup:
					raise ValueError(f"Unknown ambient motion {motion_id!r} in expression {expr_id}")

		for reaction in self.data.get("reactions", []):
			reaction_id = reaction["id"]
			base_expression = reaction.get("base_expression")
			if base_expression and base_expression not in self.expression_lookup:
				raise ValueError(f"Unknown base expression {base_expression!r} in reaction {reaction_id}")

			micro_animation = reaction.get("micro_animation")
			if micro_animation and micro_animation not in self.micro_animation_lookup:
				raise ValueError(f"Unknown micro animation {micro_animation!r} in reaction {reaction_id}")

			for key, group in (
				("left_eye_white", "eye_whites"),
				("right_eye_white", "eye_whites"),
				("left_eyeball", "pupils"),
				("right_eyeball", "pupils"),
				("left_brow", "eyebrows"),
				("right_brow", "eyebrows"),
				("mouth", "mouths"),
				("whiskers", "whiskers"),
				("indicator", "indicators"),
				("overlay", "overlays"),
				("temporary_indicator", "indicators"),
				("temporary_overlay", "overlays"),
				("temporary_left_eye_white", "eye_whites"),
				("temporary_right_eye_white", "eye_whites"),
				("temporary_left_eyeball", "pupils"),
				("temporary_right_eyeball", "pupils"),
			):
				value = reaction.get(key)
				if value:
					self._require_asset(value, group, f"reaction {reaction_id}.{key}")
					referenced_assets.add(value)

			temporary_effect = reaction.get("temporary_effect")
			if temporary_effect:
				self._require_asset(temporary_effect, "effects", f"reaction {reaction_id}.temporary_effect")
				referenced_assets.add(temporary_effect)

			for effect_entry in reaction.get("temporary_effects", []):
				effect_id = self._effect_id_from_entry(effect_entry)
				if effect_id:
					self._require_asset(effect_id, "effects", f"reaction {reaction_id}.temporary_effects")
					referenced_assets.add(effect_id)

		for record in self.asset_records:
			item = record["item"]
			asset_id = item["id"]
			file_name = item.get("file")
			if not file_name:
				continue
			asset_path = self.json_path.parent / file_name
			if asset_path.exists():
				continue
			if asset_id in referenced_assets:
				raise ValueError(f"Referenced asset {asset_id!r} is missing source file {asset_path}")
			self.warnings.append(f"Unreferenced asset {asset_id!r} is missing source file {asset_path}")

	def _eye_white_position(self, asset: dict[str, Any], side: str) -> tuple[int, int]:
		left_base_x = int(self.anchors.get("left_eye_x", 17))
		right_base_x = int(self.anchors.get("right_eye_x", 74))
		base_y = int(self.anchors.get("left_eye_y" if side == "left" else "right_eye_y", 16))
		asset_x = int(asset.get("anchor_x", left_base_x if side == "left" else right_base_x))
		asset_y = int(asset.get("anchor_y", base_y))
		x = asset_x if side == "left" else right_base_x + (asset_x - left_base_x)
		return x, asset_y

	def _slot_base_position(self, slot_key: str, asset: dict[str, Any] | None) -> tuple[int, int]:
		if slot_key == "left_brow":
			return int(self.anchors.get("brow_left_x", 46)), int(self.anchors.get("brow_left_y", 8))
		if slot_key == "right_brow":
			return int(self.anchors.get("brow_right_x", 72)), int(self.anchors.get("brow_right_y", 8))
		if slot_key == "mouth":
			return int(self.anchors.get("mouth_x", 54)), int(self.anchors.get("mouth_y", 38))
		if slot_key == "indicator":
			return int(self.anchors.get("indicator_x", 3)), int(self.anchors.get("indicator_y", 3))
		if slot_key == "overlay":
			return int(self.anchors.get("overlay_x", 3)), int(self.anchors.get("overlay_y", 3))
		if slot_key == "effect":
			return int(self.anchors.get("effect_x", 104)), int(self.anchors.get("effect_y", 6))
		if slot_key == "left_whisker":
			return WHISKER_LEFT_X, WHISKER_Y
		if slot_key == "right_whisker":
			width = int(asset.get("width", 0) if asset else 0)
			return self.canvas_width - 1 - width, WHISKER_Y
		raise ValueError(f"Unsupported slot base {slot_key}")

	def _merged_override(self, expression: dict[str, Any], reaction: dict[str, Any] | None, key: str) -> dict[str, int]:
		expr_layout = expression.get("layout_overrides", {}).get(key)
		expr_slot = expression.get("slot_overrides", {}).get(key)
		reaction_layout = reaction.get("layout_overrides", {}).get(key) if reaction else None
		reaction_slot = reaction.get("slot_overrides", {}).get(key) if reaction else None
		return merge_override(merge_override(expr_layout, expr_slot), merge_override(reaction_layout, reaction_slot))

	def _reaction_override(self, reaction: dict[str, Any], key: str) -> dict[str, int]:
		return merge_override(reaction.get("layout_overrides", {}).get(key), reaction.get("slot_overrides", {}).get(key))

	def _recipe_layout(self, expression: dict[str, Any]) -> dict[str, tuple[int, int]]:
		left_eye_asset = self._asset_item(expression["left_eye_white"])
		right_eye_asset = self._asset_item(expression["right_eye_white"])
		left_pupil_asset = self._asset_item(expression["left_eyeball"])
		right_pupil_asset = self._asset_item(expression["right_eyeball"])
		whisker_asset = self._asset_item(expression["whiskers"])

		return {
			"left_eye_white": apply_override(
				self._eye_white_position(left_eye_asset, "left"),
				self._merged_override(expression, None, "left_eye_white"),
			),
			"right_eye_white": apply_override(
				self._eye_white_position(right_eye_asset, "right"),
				self._merged_override(expression, None, "right_eye_white"),
			),
			"left_eyeball": apply_override(
				(int(left_pupil_asset.get("anchor_x", 0)), int(left_pupil_asset.get("anchor_y", 0))),
				self._merged_override(expression, None, "left_eyeball"),
			),
			"right_eyeball": apply_override(
				(int(right_pupil_asset.get("anchor_x", 0)), int(right_pupil_asset.get("anchor_y", 0))),
				self._merged_override(expression, None, "right_eyeball"),
			),
			"left_brow": apply_override(
				self._slot_base_position("left_brow", self._asset_item(expression["left_brow"])),
				self._merged_override(expression, None, "left_brow"),
			),
			"right_brow": apply_override(
				self._slot_base_position("right_brow", self._asset_item(expression["right_brow"])),
				self._merged_override(expression, None, "right_brow"),
			),
			"mouth": apply_override(
				self._slot_base_position("mouth", self._asset_item(expression["mouth"])),
				self._merged_override(expression, None, "mouth"),
			),
			"left_whisker": apply_override(
				self._slot_base_position("left_whisker", whisker_asset),
				self._merged_override(expression, None, "whiskers"),
			),
			"right_whisker": apply_override(
				self._slot_base_position("right_whisker", whisker_asset),
				self._merged_override(expression, None, "whiskers"),
			),
			"indicator": apply_override(
				self._slot_base_position("indicator", self._asset_item(expression.get("indicator") or expression.get("default_indicator"))),
				self._merged_override(expression, None, "indicator"),
			),
			"overlay": apply_override(
				self._slot_base_position("overlay", self._asset_item(expression.get("overlay") or expression.get("default_overlay"))),
				self._merged_override(expression, None, "overlay"),
			),
			"effect": apply_override(
				self._slot_base_position("effect", None),
				self._merged_override(expression, None, "effect"),
			),
		}

	def _bitmap_for_asset(self, item: dict[str, Any]) -> AssetBitmap | None:
		file_name = item.get("file")
		if not file_name:
			return None
		asset_path = self.json_path.parent / file_name
		if not asset_path.exists():
			return None
		width = int(item.get("width", 0))
		height = int(item.get("height", 0))
		if width <= 0 or height <= 0:
			return None
		return rasterize_svg_to_bitmap(
			asset_path,
			width,
			height,
			draw_black=str(item.get("color_role", "")).lower() == "foreground_black",
		)

	def generate(self) -> dict[Path, str]:
		self.validate()
		rendered_assets: dict[str, AssetBitmap | None] = {}
		max_workers = min(6, max(1, os.cpu_count() or 1), len(self.asset_records))
		with concurrent.futures.ThreadPoolExecutor(max_workers=max_workers) as executor:
			futures = {
				executor.submit(self._bitmap_for_asset, record["item"]): record["item"]["id"]
				for record in self.asset_records
			}
			for future in concurrent.futures.as_completed(futures):
				rendered_assets[futures[future]] = future.result()

		return {
			DEFAULT_OUT_INCLUDE / "kerfur_face_assets.h": self._render_assets_h(),
			DEFAULT_OUT_SRC / "kerfur_face_assets.c": self._render_assets_c(rendered_assets),
			DEFAULT_OUT_INCLUDE / "kerfur_face_recipes.h": self._render_recipes_h(),
			DEFAULT_OUT_SRC / "kerfur_face_recipes.c": self._render_recipes_c(),
		}

	def _render_assets_h(self) -> str:
		group_order = [
			"eye_whites",
			"pupils",
			"eyebrows",
			"mouths",
			"whiskers",
			"effects",
			"indicators",
			"overlays",
		]
		asset_enum_lines = ["\tKERFUR_FACE_ASSET_NONE = 0,"]
		for record in self.asset_records:
			asset_enum_lines.append(f"\t{make_enum('KERFUR_FACE_ASSET', record['item']['id'])},")

		indicator_lines = ["\tKERFUR_FACE_INDICATOR_NONE = 0,"]
		for item in self.assets_by_group.get("indicators", []):
			indicator_lines.append(f"\t{make_enum('KERFUR_FACE_INDICATOR', item['id'])},")

		overlay_lines = ["\tKERFUR_FACE_OVERLAY_NONE = 0,"]
		for item in self.assets_by_group.get("overlays", []):
			overlay_lines.append(f"\t{make_enum('KERFUR_FACE_OVERLAY', item['id'])},")

		group_lines = [f"\t{make_enum('KERFUR_FACE_GROUP', group_name)}," for group_name in group_order]

		return f"""#ifndef KERFUR_FACE_ASSETS_H_
#define KERFUR_FACE_ASSETS_H_

#include <stdint.h>

#include "ui/face_bitmap.h"

enum kerfur_face_asset_group {{
{chr(10).join(group_lines)}
}};

enum kerfur_face_overlay_render_mode {{
\tKERFUR_FACE_OVERLAY_RENDER_NONE = 0,
\tKERFUR_FACE_OVERLAY_RENDER_BITMAP,
\tKERFUR_FACE_OVERLAY_RENDER_BATTERY_PERCENT_TEXT,
}};

enum kerfur_face_pupil_zone_shape {{
\tKERFUR_FACE_PUPIL_ZONE_NONE = 0,
\tKERFUR_FACE_PUPIL_ZONE_ELLIPSE,
\tKERFUR_FACE_PUPIL_ZONE_RECTANGLE,
}};

enum kerfur_face_asset_flags {{
\tKERFUR_FACE_ASSET_FLAG_MIRRORABLE = 1 << 0,
\tKERFUR_FACE_ASSET_FLAG_MIRROR_RIGHT = 1 << 1,
\tKERFUR_FACE_ASSET_FLAG_SPECIAL_MODE = 1 << 2,
\tKERFUR_FACE_ASSET_FLAG_DRAW_BLACK = 1 << 3,
}};

enum kerfur_face_asset_id {{
{chr(10).join(asset_enum_lines)}
\tKERFUR_FACE_ASSET_COUNT
}};

enum kerfur_face_indicator_id {{
{chr(10).join(indicator_lines)}
\tKERFUR_FACE_INDICATOR_COUNT
}};

enum kerfur_face_overlay_id {{
{chr(10).join(overlay_lines)}
\tKERFUR_FACE_OVERLAY_COUNT
}};

struct kerfur_face_eye_zone {{
\tuint8_t shape;
\tint16_t center_x;
\tint16_t center_y;
\tint16_t rx;
\tint16_t ry;
\tint16_t margin;
}};

struct kerfur_face_asset_metadata {{
\tenum kerfur_face_asset_id id;
\tenum kerfur_face_asset_group group;
\tconst char *name;
\tconst struct kerfur_face_bitmap *bitmap;
\tint16_t anchor_x;
\tint16_t anchor_y;
\tuint8_t flags;
\tstruct kerfur_face_eye_zone pupil_zone;
\tuint8_t overlay_render_mode;
}};

struct kerfur_face_indicator_def {{
\tenum kerfur_face_indicator_id id;
\tconst char *name;
\tenum kerfur_face_asset_id asset_id;
}};

struct kerfur_face_overlay_def {{
\tenum kerfur_face_overlay_id id;
\tconst char *name;
\tenum kerfur_face_asset_id asset_id;
\tenum kerfur_face_overlay_render_mode render_mode;
}};

const struct kerfur_face_asset_metadata *kerfur_face_asset_get(enum kerfur_face_asset_id id);
const struct kerfur_face_indicator_def *kerfur_face_indicator_get(enum kerfur_face_indicator_id id);
const struct kerfur_face_overlay_def *kerfur_face_overlay_get(enum kerfur_face_overlay_id id);
const char *kerfur_face_asset_name(enum kerfur_face_asset_id id);
const char *kerfur_face_indicator_name(enum kerfur_face_indicator_id id);
const char *kerfur_face_overlay_name(enum kerfur_face_overlay_id id);

#endif /* KERFUR_FACE_ASSETS_H_ */
"""

	def _render_assets_c(self, rendered_assets: dict[str, AssetBitmap | None]) -> str:
		asset_bits_blocks: list[str] = []
		metadata_lines: list[str] = ["\t[KERFUR_FACE_ASSET_NONE] = {0},"]
		indicator_lines: list[str] = ["\t[KERFUR_FACE_INDICATOR_NONE] = {0},"]
		overlay_lines: list[str] = ["\t[KERFUR_FACE_OVERLAY_NONE] = {0},"]

		group_enum = {
			"eye_whites": "KERFUR_FACE_GROUP_EYE_WHITES",
			"pupils": "KERFUR_FACE_GROUP_PUPILS",
			"eyebrows": "KERFUR_FACE_GROUP_EYEBROWS",
			"mouths": "KERFUR_FACE_GROUP_MOUTHS",
			"whiskers": "KERFUR_FACE_GROUP_WHISKERS",
			"effects": "KERFUR_FACE_GROUP_EFFECTS",
			"indicators": "KERFUR_FACE_GROUP_INDICATORS",
			"overlays": "KERFUR_FACE_GROUP_OVERLAYS",
		}

		for record in self.asset_records:
			group_name = record["group"]
			item = record["item"]
			asset_id = item["id"]
			enum_name = make_enum("KERFUR_FACE_ASSET", asset_id)
			symbol = make_symbol("g_face_asset", asset_id)
			bitmap = rendered_assets[asset_id]
			if bitmap is not None and bitmap.data:
				asset_bits_blocks.append(
					f"static const uint8_t {symbol}_bits[] = {format_c_array(bitmap.data)};\n"
					f"static const struct kerfur_face_bitmap {symbol}_bitmap = {{\n"
					f"\t.width = {bitmap.width},\n"
					f"\t.height = {bitmap.height},\n"
					f"\t.stride = {bitmap.stride},\n"
					f"\t.data = {symbol}_bits,\n"
					f"}};\n"
				)
				bitmap_ref = f"&{symbol}_bitmap"
			else:
				bitmap_ref = "NULL"

			zone = item.get("pupil_zone") or item.get("eye_solver") or {}
			shape = str(zone.get("shape", "none")).lower()
			if shape == "ellipse":
				zone_shape = "KERFUR_FACE_PUPIL_ZONE_ELLIPSE"
			elif shape == "rectangle":
				zone_shape = "KERFUR_FACE_PUPIL_ZONE_RECTANGLE"
			else:
				zone_shape = "KERFUR_FACE_PUPIL_ZONE_NONE"

			center = zone.get("center", {})
			zone_center_x = int(zone.get("center_x", center.get("x", int(item.get("width", 0)) // 2)))
			zone_center_y = int(zone.get("center_y", center.get("y", int(item.get("height", 0)) // 2)))
			flags = 0
			if item.get("mirrorable"):
				flags |= 1
			if item.get("mirror_right"):
				flags |= 2
			if item.get("special_mode"):
				flags |= 4
			if str(item.get("color_role", "")).lower() == "foreground_black":
				flags |= 8

			render_mode = "KERFUR_FACE_OVERLAY_RENDER_NONE"
			if group_name == "overlays":
				item_render_mode = str(item.get("render_mode", "")).lower()
				if item_render_mode == "text_runtime":
					render_mode = "KERFUR_FACE_OVERLAY_RENDER_BATTERY_PERCENT_TEXT"
				elif item.get("file"):
					render_mode = "KERFUR_FACE_OVERLAY_RENDER_BITMAP"

			metadata_lines.append(
				f"\t[{enum_name}] = {{\n"
				f"\t\t.id = {enum_name},\n"
				f"\t\t.group = {group_enum[group_name]},\n"
				f"\t\t.name = {format_c_string(asset_id)},\n"
				f"\t\t.bitmap = {bitmap_ref},\n"
				f"\t\t.anchor_x = {int(item.get('anchor_x', 0))},\n"
				f"\t\t.anchor_y = {int(item.get('anchor_y', 0))},\n"
				f"\t\t.flags = {flags},\n"
				f"\t\t.pupil_zone = {{ {zone_shape}, {zone_center_x}, {zone_center_y}, {int(zone.get('rx', 0))}, {int(zone.get('ry', 0))}, {int(zone.get('margin', 0))} }},\n"
				f"\t\t.overlay_render_mode = {render_mode},\n"
				f"\t}},"
			)

		for item in self.assets_by_group.get("indicators", []):
			indicator_lines.append(
				f"\t[{make_enum('KERFUR_FACE_INDICATOR', item['id'])}] = {{ "
				f"{make_enum('KERFUR_FACE_INDICATOR', item['id'])}, {format_c_string(item['id'])}, {make_enum('KERFUR_FACE_ASSET', item['id'])} }},"
			)

		for item in self.assets_by_group.get("overlays", []):
			render_mode = "KERFUR_FACE_OVERLAY_RENDER_BITMAP"
			if str(item.get("render_mode", "")).lower() == "text_runtime":
				render_mode = "KERFUR_FACE_OVERLAY_RENDER_BATTERY_PERCENT_TEXT"
			elif not item.get("file"):
				render_mode = "KERFUR_FACE_OVERLAY_RENDER_NONE"
			overlay_lines.append(
				f"\t[{make_enum('KERFUR_FACE_OVERLAY', item['id'])}] = {{ "
				f"{make_enum('KERFUR_FACE_OVERLAY', item['id'])}, {format_c_string(item['id'])}, {make_enum('KERFUR_FACE_ASSET', item['id'])}, {render_mode} }},"
			)

		return f"""#include <stddef.h>

#include "ui/generated/kerfur_face_assets.h"

{chr(10).join(asset_bits_blocks)}
static const struct kerfur_face_asset_metadata g_face_assets[KERFUR_FACE_ASSET_COUNT] = {{
{chr(10).join(metadata_lines)}
}};

static const struct kerfur_face_indicator_def g_face_indicators[KERFUR_FACE_INDICATOR_COUNT] = {{
{chr(10).join(indicator_lines)}
}};

static const struct kerfur_face_overlay_def g_face_overlays[KERFUR_FACE_OVERLAY_COUNT] = {{
{chr(10).join(overlay_lines)}
}};

const struct kerfur_face_asset_metadata *kerfur_face_asset_get(enum kerfur_face_asset_id id)
{{
\tif ((id <= KERFUR_FACE_ASSET_NONE) || (id >= KERFUR_FACE_ASSET_COUNT)) {{
\t\treturn &g_face_assets[KERFUR_FACE_ASSET_NONE];
\t}}
\treturn &g_face_assets[id];
}}

const struct kerfur_face_indicator_def *kerfur_face_indicator_get(enum kerfur_face_indicator_id id)
{{
\tif ((id <= KERFUR_FACE_INDICATOR_NONE) || (id >= KERFUR_FACE_INDICATOR_COUNT)) {{
\t\treturn &g_face_indicators[KERFUR_FACE_INDICATOR_NONE];
\t}}
\treturn &g_face_indicators[id];
}}

const struct kerfur_face_overlay_def *kerfur_face_overlay_get(enum kerfur_face_overlay_id id)
{{
\tif ((id <= KERFUR_FACE_OVERLAY_NONE) || (id >= KERFUR_FACE_OVERLAY_COUNT)) {{
\t\treturn &g_face_overlays[KERFUR_FACE_OVERLAY_NONE];
\t}}
\treturn &g_face_overlays[id];
}}

const char *kerfur_face_asset_name(enum kerfur_face_asset_id id)
{{
\treturn kerfur_face_asset_get(id)->name;
}}

const char *kerfur_face_indicator_name(enum kerfur_face_indicator_id id)
{{
\treturn kerfur_face_indicator_get(id)->name;
}}

const char *kerfur_face_overlay_name(enum kerfur_face_overlay_id id)
{{
\treturn kerfur_face_overlay_get(id)->name;
}}
"""

	def _render_recipes_h(self) -> str:
		blink_lines = ["\tKERFUR_FACE_BLINK_PROFILE_NONE = 0,"]
		for item in self.data.get("blink_profiles", []):
			blink_lines.append(f"\t{make_enum('KERFUR_FACE_BLINK_PROFILE', item['id'])},")

		micro_lines = ["\tKERFUR_FACE_MICRO_ANIM_NONE = 0,"]
		for item in self.data.get("micro_animations", []):
			micro_lines.append(f"\t{make_enum('KERFUR_FACE_MICRO_ANIM', item['id'])},")

		recipe_alias_lines = [
			f"\t{make_enum('KERFUR_FACE_RECIPE', expr)} = {expr},"
			for expr in self.pet_expressions
		]
		reaction_alias_lines = [
			f"\t{make_enum('KERFUR_FACE_REACTION', reaction)} = {reaction},"
			for reaction in self.micro_reactions
		]

		return f"""#ifndef KERFUR_FACE_RECIPES_H_
#define KERFUR_FACE_RECIPES_H_

#include <stdbool.h>
#include <stdint.h>

#include "behavior/micro_reaction.h"
#include "behavior/pet_state.h"
#include "ui/generated/kerfur_face_assets.h"

#define KERFUR_FACE_MAX_EFFECTS {MAX_EFFECTS}

enum kerfur_face_recipe_id {{
{chr(10).join(recipe_alias_lines)}
\tKERFUR_FACE_RECIPE_COUNT = PET_EXPR_ASLEEP + 1
}};

enum kerfur_face_reaction_id {{
{chr(10).join(reaction_alias_lines)}
}};

enum kerfur_face_blink_profile_id {{
{chr(10).join(blink_lines)}
\tKERFUR_FACE_BLINK_PROFILE_COUNT
}};

enum kerfur_face_micro_anim_kind {{
\tKERFUR_FACE_MICRO_ANIM_KIND_NONE = 0,
\tKERFUR_FACE_MICRO_ANIM_KIND_LOOK_TARGET,
\tKERFUR_FACE_MICRO_ANIM_KIND_PUPIL_MOTION,
\tKERFUR_FACE_MICRO_ANIM_KIND_EFFECT_SPAWN,
}};

enum kerfur_face_micro_anim_id {{
{chr(10).join(micro_lines)}
\tKERFUR_FACE_MICRO_ANIM_COUNT
}};

enum kerfur_face_reaction_compose_mode {{
\tKERFUR_FACE_REACTION_COMPOSE_OVERLAY = 0,
\tKERFUR_FACE_REACTION_COMPOSE_REBASE,
}};

enum kerfur_face_override_flags {{
\tKERFUR_FACE_OVERRIDE_HAS_X = 1 << 0,
\tKERFUR_FACE_OVERRIDE_HAS_Y = 1 << 1,
}};

struct kerfur_face_point {{
\tint16_t x;
\tint16_t y;
}};

struct kerfur_face_override {{
\tuint8_t flags;
\tint16_t x;
\tint16_t y;
\tint16_t dx;
\tint16_t dy;
}};

struct kerfur_face_layout {{
\tstruct kerfur_face_point left_eye_white;
\tstruct kerfur_face_point right_eye_white;
\tstruct kerfur_face_point left_brow;
\tstruct kerfur_face_point right_brow;
\tstruct kerfur_face_point mouth;
\tstruct kerfur_face_point left_whisker;
\tstruct kerfur_face_point right_whisker;
\tstruct kerfur_face_point indicator;
\tstruct kerfur_face_point overlay;
\tstruct kerfur_face_point effect;
\tstruct kerfur_face_point left_eyeball;
\tstruct kerfur_face_point right_eyeball;
}};

struct kerfur_face_effect_instance {{
\tenum kerfur_face_asset_id asset_id;
\tbool has_absolute_position;
\tint16_t x;
\tint16_t y;
\tint16_t dx;
\tint16_t dy;
}};

struct kerfur_face_recipe {{
\tconst char *name;
\tenum kerfur_face_asset_id left_eye_white;
\tenum kerfur_face_asset_id right_eye_white;
\tenum kerfur_face_asset_id blink_left_eye_white;
\tenum kerfur_face_asset_id blink_right_eye_white;
\tenum kerfur_face_asset_id left_eyeball;
\tenum kerfur_face_asset_id right_eyeball;
\tenum kerfur_face_asset_id left_brow;
\tenum kerfur_face_asset_id right_brow;
\tenum kerfur_face_asset_id mouth;
\tenum kerfur_face_asset_id whiskers;
\tenum kerfur_face_indicator_id default_indicator;
\tenum kerfur_face_overlay_id default_overlay;
\tenum kerfur_face_blink_profile_id blink_profile;
\tstruct kerfur_face_layout layout;
\tint16_t base_look_x;
\tint16_t base_look_y;
\tuint8_t allow_dynamic_pupils;
\tuint8_t special_eye_mode;
\tuint8_t dynamic_pupil_scale_x;
\tuint8_t dynamic_pupil_scale_y;
\tuint8_t dynamic_pupil_speed;
\tuint8_t dead_zone_strength;
\tuint8_t base_eye_openness;
\tuint8_t transition_speed;
\tuint8_t pupil_swap_style;
\tuint8_t ambient_motion_count;
\tconst enum kerfur_face_micro_anim_id *ambient_motion;
\tuint8_t default_effect_count;
\tconst enum kerfur_face_asset_id *default_effects;
}};

struct kerfur_face_micro_animation {{
\tconst char *name;
\tenum kerfur_face_micro_anim_kind kind;
\tuint16_t duration_ms;
\tbool loop;
\tint16_t target_x;
\tint16_t target_y;
\tint16_t radius_x;
\tint16_t radius_y;
\tenum kerfur_face_asset_id effect_asset_id;
\tint16_t offset_x;
\tint16_t offset_y;
}};

struct kerfur_face_reaction {{
\tconst char *name;
\tuint16_t duration_ms;
\tenum kerfur_face_reaction_compose_mode compose_mode;
\tenum pet_expression base_expression;
\tint16_t look_offset_x;
\tint16_t look_offset_y;
\tbool has_allow_dynamic_pupils_override;
\tbool allow_dynamic_pupils;
\tenum kerfur_face_asset_id left_eye_white;
\tenum kerfur_face_asset_id right_eye_white;
\tenum kerfur_face_asset_id left_eyeball;
\tenum kerfur_face_asset_id right_eyeball;
\tenum kerfur_face_asset_id left_brow;
\tenum kerfur_face_asset_id right_brow;
\tenum kerfur_face_asset_id mouth;
\tenum kerfur_face_asset_id whiskers;
\tenum kerfur_face_indicator_id indicator;
\tenum kerfur_face_overlay_id overlay;
\tenum kerfur_face_blink_profile_id blink_override;
\tenum kerfur_face_micro_anim_id micro_animation;
\tstruct kerfur_face_override left_eye_white_override;
\tstruct kerfur_face_override right_eye_white_override;
\tstruct kerfur_face_override left_eyeball_override;
\tstruct kerfur_face_override right_eyeball_override;
\tstruct kerfur_face_override left_brow_override;
\tstruct kerfur_face_override right_brow_override;
\tstruct kerfur_face_override mouth_override;
\tstruct kerfur_face_override whiskers_override;
\tstruct kerfur_face_override indicator_override;
\tstruct kerfur_face_override overlay_override;
\tstruct kerfur_face_override effect_override;
\tuint8_t temporary_effect_count;
\tstruct kerfur_face_effect_instance temporary_effects[KERFUR_FACE_MAX_EFFECTS];
\tbool has_whisker_wiggle;
\tuint16_t stagger_delay_ms;
\tuint8_t pupil_swap_override;
}};

const struct kerfur_face_recipe *kerfur_face_recipe_get(enum kerfur_face_recipe_id id);
const struct kerfur_face_reaction *kerfur_face_reaction_get(enum kerfur_face_reaction_id id);
const struct kerfur_face_micro_animation *kerfur_face_micro_anim_get(enum kerfur_face_micro_anim_id id);
const char *kerfur_face_recipe_name(enum kerfur_face_recipe_id id);
const char *kerfur_face_reaction_name(enum kerfur_face_reaction_id id);

#endif /* KERFUR_FACE_RECIPES_H_ */
"""

	def _default_blink_eye(self, expr: dict[str, Any], side: str) -> str:
		eye_key = "left_eye_white" if side == "left" else "right_eye_white"
		eye_white = str(expr.get(eye_key, ""))
		if eye_white in {"eye_white_blink", "eye_white_blink_crying"}:
			return eye_white
		if expr["id"] in {"PET_EXPR_NEEDY", "PET_EXPR_LONELY"}:
			return "eye_white_blink_crying"
		return "eye_white_blink"

	def _asset_override_enum(self, reaction: dict[str, Any], keys: list[str]) -> str:
		for key in keys:
			value = reaction.get(key)
			if value:
				return make_enum("KERFUR_FACE_ASSET", value)
		return "KERFUR_FACE_ASSET_NONE"

	def _indicator_override_enum(self, reaction: dict[str, Any]) -> str:
		value = reaction.get("indicator") or reaction.get("temporary_indicator")
		return make_enum("KERFUR_FACE_INDICATOR", value) if value else "KERFUR_FACE_INDICATOR_NONE"

	def _overlay_override_enum(self, reaction: dict[str, Any]) -> str:
		value = reaction.get("overlay") or reaction.get("temporary_overlay")
		return make_enum("KERFUR_FACE_OVERLAY", value) if value else "KERFUR_FACE_OVERLAY_NONE"

	def _format_override(self, override: dict[str, int]) -> str:
		flags = 0
		if "x" in override:
			flags |= 1
		if "y" in override:
			flags |= 2
		return "{ %d, %d, %d, %d, %d }" % (
			flags,
			int(override.get("x", 0)),
			int(override.get("y", 0)),
			int(override.get("dx", 0)),
			int(override.get("dy", 0)),
		)

	def _pupil_swap_enum(self, style: str | None) -> str:
		if style is None:
			return "0"
		return str(PUPIL_SWAP_MAP.get(str(style).lower(), 0))

	def _render_reaction_effects(self, reaction: dict[str, Any]) -> list[dict[str, Any]]:
		effects: list[dict[str, Any]] = []
		if reaction.get("temporary_effect"):
			effects.append(
				{
					"asset_id": reaction["temporary_effect"],
					"has_absolute_position": False,
					"x": 0,
					"y": 0,
					"dx": 0,
					"dy": 0,
				}
			)
		for entry in reaction.get("temporary_effects", []):
			effect_id = self._effect_id_from_entry(entry)
			if not effect_id:
				continue
			if isinstance(entry, dict) and ("x" in entry or "y" in entry):
				effects.append(
					{
						"asset_id": effect_id,
						"has_absolute_position": True,
						"x": int(entry.get("x", 0)),
						"y": int(entry.get("y", 0)),
						"dx": int(entry.get("dx", 0)),
						"dy": int(entry.get("dy", 0)),
					}
				)
			else:
				effects.append(
					{
						"asset_id": effect_id,
						"has_absolute_position": False,
						"x": 0,
						"y": 0,
						"dx": int(entry.get("dx", 0)) if isinstance(entry, dict) else 0,
						"dy": int(entry.get("dy", 0)) if isinstance(entry, dict) else 0,
					}
				)
		return effects

	def _render_recipes_c(self) -> str:
		ambient_defs: list[str] = []
		default_effect_defs: list[str] = []
		recipe_lines: list[str] = []
		micro_lines: list[str] = ["\t[KERFUR_FACE_MICRO_ANIM_NONE] = {0},"]
		reaction_lines: list[str] = ["\t[KERFUR_FACE_REACTION_REACTION_NONE] = {0},"]

		for expr in self.data.get("expressions", []):
			expr_id = expr["id"]
			enum_name = make_enum("KERFUR_FACE_RECIPE", expr_id)
			layout = self._recipe_layout(expr)
			ambient = expr.get("ambient_motion", [])
			default_effects = expr.get("default_effects", [])

			ambient_ref = "NULL"
			if ambient:
				symbol = make_symbol("g_face_ambient_motion", expr_id)
				ambient_ref = symbol
				ambient_defs.append(
					f"static const enum kerfur_face_micro_anim_id {symbol}[] = {{ "
					+ ", ".join(make_enum("KERFUR_FACE_MICRO_ANIM", item) for item in ambient)
					+ " };"
				)

			default_effect_ref = "NULL"
			if default_effects:
				symbol = make_symbol("g_face_default_effects", expr_id)
				default_effect_ref = symbol
				default_effect_defs.append(
					f"static const enum kerfur_face_asset_id {symbol}[] = {{ "
					+ ", ".join(make_enum("KERFUR_FACE_ASSET", item) for item in default_effects)
					+ " };"
				)

			blink_left = expr.get("blink_left_eye_white") or self._default_blink_eye(expr, "left")
			blink_right = expr.get("blink_right_eye_white") or self._default_blink_eye(expr, "right")
			indicator = expr.get("default_indicator") or expr.get("indicator")
			overlay = expr.get("default_overlay") or expr.get("overlay")
			blink_profile = expr.get("blink_profile")

			recipe_lines.append(
				f"\t[{enum_name}] = {{\n"
				f"\t\t.name = {format_c_string(expr_id)},\n"
				f"\t\t.left_eye_white = {make_enum('KERFUR_FACE_ASSET', expr['left_eye_white'])},\n"
				f"\t\t.right_eye_white = {make_enum('KERFUR_FACE_ASSET', expr['right_eye_white'])},\n"
				f"\t\t.blink_left_eye_white = {make_enum('KERFUR_FACE_ASSET', blink_left)},\n"
				f"\t\t.blink_right_eye_white = {make_enum('KERFUR_FACE_ASSET', blink_right)},\n"
				f"\t\t.left_eyeball = {make_enum('KERFUR_FACE_ASSET', expr['left_eyeball'])},\n"
				f"\t\t.right_eyeball = {make_enum('KERFUR_FACE_ASSET', expr['right_eyeball'])},\n"
				f"\t\t.left_brow = {make_enum('KERFUR_FACE_ASSET', expr['left_brow'])},\n"
				f"\t\t.right_brow = {make_enum('KERFUR_FACE_ASSET', expr['right_brow'])},\n"
				f"\t\t.mouth = {make_enum('KERFUR_FACE_ASSET', expr['mouth'])},\n"
				f"\t\t.whiskers = {make_enum('KERFUR_FACE_ASSET', expr['whiskers'])},\n"
				f"\t\t.default_indicator = {make_enum('KERFUR_FACE_INDICATOR', indicator) if indicator else 'KERFUR_FACE_INDICATOR_NONE'},\n"
				f"\t\t.default_overlay = {make_enum('KERFUR_FACE_OVERLAY', overlay) if overlay else 'KERFUR_FACE_OVERLAY_NONE'},\n"
				f"\t\t.blink_profile = {make_enum('KERFUR_FACE_BLINK_PROFILE', blink_profile) if blink_profile else 'KERFUR_FACE_BLINK_PROFILE_NONE'},\n"
				f"\t\t.layout = {{\n"
				f"\t\t\t.left_eye_white = {{ {layout['left_eye_white'][0]}, {layout['left_eye_white'][1]} }},\n"
				f"\t\t\t.right_eye_white = {{ {layout['right_eye_white'][0]}, {layout['right_eye_white'][1]} }},\n"
				f"\t\t\t.left_brow = {{ {layout['left_brow'][0]}, {layout['left_brow'][1]} }},\n"
				f"\t\t\t.right_brow = {{ {layout['right_brow'][0]}, {layout['right_brow'][1]} }},\n"
				f"\t\t\t.mouth = {{ {layout['mouth'][0]}, {layout['mouth'][1]} }},\n"
				f"\t\t\t.left_whisker = {{ {layout['left_whisker'][0]}, {layout['left_whisker'][1]} }},\n"
				f"\t\t\t.right_whisker = {{ {layout['right_whisker'][0]}, {layout['right_whisker'][1]} }},\n"
				f"\t\t\t.indicator = {{ {layout['indicator'][0]}, {layout['indicator'][1]} }},\n"
				f"\t\t\t.overlay = {{ {layout['overlay'][0]}, {layout['overlay'][1]} }},\n"
				f"\t\t\t.effect = {{ {layout['effect'][0]}, {layout['effect'][1]} }},\n"
				f"\t\t\t.left_eyeball = {{ {layout['left_eyeball'][0]}, {layout['left_eyeball'][1]} }},\n"
				f"\t\t\t.right_eyeball = {{ {layout['right_eyeball'][0]}, {layout['right_eyeball'][1]} }},\n"
				f"\t\t}},\n"
				f"\t\t.base_look_x = {int(expr.get('base_look', {}).get('x', 0))},\n"
				f"\t\t.base_look_y = {int(expr.get('base_look', {}).get('y', 0))},\n"
				f"\t\t.allow_dynamic_pupils = {1 if expr.get('allow_dynamic_pupils', True) else 0},\n"
				f"\t\t.special_eye_mode = {1 if expr.get('special_eye_mode', False) else 0},\n"
				f"\t\t.dynamic_pupil_scale_x = {int(expr.get('dynamic_pupil_scale_x', 100))},\n"
				f"\t\t.dynamic_pupil_scale_y = {int(expr.get('dynamic_pupil_scale_y', 100))},\n"
				f"\t\t.dynamic_pupil_speed = {int(expr.get('dynamic_pupil_speed', 32))},\n"
				f"\t\t.dead_zone_strength = {int(expr.get('dead_zone_strength', 0))},\n"
				f"\t\t.base_eye_openness = {int(expr.get('base_eye_openness', 100))},\n"
				f"\t\t.transition_speed = {int(expr.get('transition_speed', 40))},\n"
				f"\t\t.pupil_swap_style = {self._pupil_swap_enum(expr.get('pupil_swap_style', 'instant'))},\n"
				f"\t\t.ambient_motion_count = {len(ambient)},\n"
				f"\t\t.ambient_motion = {ambient_ref},\n"
				f"\t\t.default_effect_count = {len(default_effects)},\n"
				f"\t\t.default_effects = {default_effect_ref},\n"
				f"\t}},"
			)

		for anim in self.data.get("micro_animations", []):
			params = anim.get("params", {})
			kind = {
				"look_target": "KERFUR_FACE_MICRO_ANIM_KIND_LOOK_TARGET",
				"pupil_motion": "KERFUR_FACE_MICRO_ANIM_KIND_PUPIL_MOTION",
				"effect_spawn": "KERFUR_FACE_MICRO_ANIM_KIND_EFFECT_SPAWN",
			}[anim.get("type", "look_target")]
			effect_asset = params.get("effect_id")
			micro_lines.append(
				f"\t[{make_enum('KERFUR_FACE_MICRO_ANIM', anim['id'])}] = {{\n"
				f"\t\t.name = {format_c_string(anim['id'])},\n"
				f"\t\t.kind = {kind},\n"
				f"\t\t.duration_ms = {int(anim.get('duration_ms', 0))},\n"
				f"\t\t.loop = {'true' if anim.get('loop', False) else 'false'},\n"
				f"\t\t.target_x = {int(params.get('target_x', 0))},\n"
				f"\t\t.target_y = {int(params.get('target_y', 0))},\n"
				f"\t\t.radius_x = {int(params.get('radius_x', 0))},\n"
				f"\t\t.radius_y = {int(params.get('radius_y', 0))},\n"
				f"\t\t.effect_asset_id = {make_enum('KERFUR_FACE_ASSET', effect_asset) if effect_asset else 'KERFUR_FACE_ASSET_NONE'},\n"
				f"\t\t.offset_x = {int(params.get('offset_x', 0))},\n"
				f"\t\t.offset_y = {int(params.get('offset_y', 0))},\n"
				f"\t}},"
			)

		for reaction in self.data.get("reactions", [])[1:]:
			enum_name = make_enum("KERFUR_FACE_REACTION", reaction["id"])
			effects = self._render_reaction_effects(reaction)
			if len(effects) > MAX_EFFECTS:
				raise ValueError(f"Reaction {reaction['id']} uses {len(effects)} effects, max is {MAX_EFFECTS}")
			effect_lines = []
			for effect in effects:
				effect_lines.append(
					"{ "
					f"{make_enum('KERFUR_FACE_ASSET', effect['asset_id'])}, "
					f"{'true' if effect['has_absolute_position'] else 'false'}, "
					f"{effect['x']}, {effect['y']}, {effect['dx']}, {effect['dy']} "
					"}"
				)
			while len(effect_lines) < MAX_EFFECTS:
				effect_lines.append("{ KERFUR_FACE_ASSET_NONE, false, 0, 0, 0, 0 }")

			compose_mode = "KERFUR_FACE_REACTION_COMPOSE_REBASE" if str(reaction.get("compose_mode", "")).lower() == "rebase" else "KERFUR_FACE_REACTION_COMPOSE_OVERLAY"
			dynamic_override = reaction.get("allow_dynamic_pupils")
			reaction_lines.append(
				f"\t[{enum_name}] = {{\n"
				f"\t\t.name = {format_c_string(reaction['id'])},\n"
				f"\t\t.duration_ms = {int(reaction.get('duration_ms', 0))},\n"
				f"\t\t.compose_mode = {compose_mode},\n"
				f"\t\t.base_expression = {reaction.get('base_expression', 'PET_EXPR_CALM')},\n"
				f"\t\t.look_offset_x = {int(reaction.get('look_offset', {}).get('x', 0))},\n"
				f"\t\t.look_offset_y = {int(reaction.get('look_offset', {}).get('y', 0))},\n"
				f"\t\t.has_allow_dynamic_pupils_override = {'true' if dynamic_override is not None else 'false'},\n"
				f"\t\t.allow_dynamic_pupils = {'true' if reaction.get('allow_dynamic_pupils', False) else 'false'},\n"
				f"\t\t.left_eye_white = {self._asset_override_enum(reaction, ['left_eye_white', 'temporary_left_eye_white'])},\n"
				f"\t\t.right_eye_white = {self._asset_override_enum(reaction, ['right_eye_white', 'temporary_right_eye_white'])},\n"
				f"\t\t.left_eyeball = {self._asset_override_enum(reaction, ['left_eyeball', 'temporary_left_eyeball'])},\n"
				f"\t\t.right_eyeball = {self._asset_override_enum(reaction, ['right_eyeball', 'temporary_right_eyeball'])},\n"
				f"\t\t.left_brow = {self._asset_override_enum(reaction, ['left_brow'])},\n"
				f"\t\t.right_brow = {self._asset_override_enum(reaction, ['right_brow'])},\n"
				f"\t\t.mouth = {self._asset_override_enum(reaction, ['mouth'])},\n"
				f"\t\t.whiskers = {self._asset_override_enum(reaction, ['whiskers'])},\n"
				f"\t\t.indicator = {self._indicator_override_enum(reaction)},\n"
				f"\t\t.overlay = {self._overlay_override_enum(reaction)},\n"
				f"\t\t.blink_override = {make_enum('KERFUR_FACE_BLINK_PROFILE', reaction['blink_override']) if reaction.get('blink_override') else 'KERFUR_FACE_BLINK_PROFILE_NONE'},\n"
				f"\t\t.micro_animation = {make_enum('KERFUR_FACE_MICRO_ANIM', reaction['micro_animation']) if reaction.get('micro_animation') else 'KERFUR_FACE_MICRO_ANIM_NONE'},\n"
				f"\t\t.left_eye_white_override = {self._format_override(self._reaction_override(reaction, 'left_eye_white'))},\n"
				f"\t\t.right_eye_white_override = {self._format_override(self._reaction_override(reaction, 'right_eye_white'))},\n"
				f"\t\t.left_eyeball_override = {self._format_override(self._reaction_override(reaction, 'left_eyeball'))},\n"
				f"\t\t.right_eyeball_override = {self._format_override(self._reaction_override(reaction, 'right_eyeball'))},\n"
				f"\t\t.left_brow_override = {self._format_override(self._reaction_override(reaction, 'left_brow'))},\n"
				f"\t\t.right_brow_override = {self._format_override(self._reaction_override(reaction, 'right_brow'))},\n"
				f"\t\t.mouth_override = {self._format_override(self._reaction_override(reaction, 'mouth'))},\n"
				f"\t\t.whiskers_override = {self._format_override(self._reaction_override(reaction, 'whiskers'))},\n"
				f"\t\t.indicator_override = {self._format_override(self._reaction_override(reaction, 'indicator'))},\n"
				f"\t\t.overlay_override = {self._format_override(self._reaction_override(reaction, 'overlay'))},\n"
				f"\t\t.effect_override = {self._format_override(self._reaction_override(reaction, 'effect'))},\n"
				f"\t\t.temporary_effect_count = {len(effects)},\n"
				f"\t\t.temporary_effects = {{ {', '.join(effect_lines)} }},\n"
				f"\t\t.has_whisker_wiggle = {'true' if reaction.get('whisker_wiggle', False) else 'false'},\n"
				f"\t\t.stagger_delay_ms = {int(reaction.get('stagger_delay_ms', 0))},\n"
				f"\t\t.pupil_swap_override = {self._pupil_swap_enum(reaction.get('pupil_swap_override'))},\n"
				f"\t}},"
			)

		return f"""#include <stddef.h>

#include "ui/generated/kerfur_face_recipes.h"

_Static_assert((int)KERFUR_FACE_RECIPE_PET_EXPR_CALM == (int)PET_EXPR_CALM, "recipe alias drift");
_Static_assert(KERFUR_FACE_RECIPE_COUNT == (PET_EXPR_ASLEEP + 1), "recipe alias drift");
_Static_assert((int)KERFUR_FACE_REACTION_REACTION_NONE == (int)REACTION_NONE, "reaction alias drift");
_Static_assert((int)KERFUR_FACE_REACTION_REACTION_LOOK_DOWN == (int)REACTION_LOOK_DOWN, "reaction alias drift");

{chr(10).join(ambient_defs)}

{chr(10).join(default_effect_defs)}

static const struct kerfur_face_recipe g_face_recipes[KERFUR_FACE_RECIPE_COUNT] = {{
{chr(10).join(recipe_lines)}
}};

static const struct kerfur_face_micro_animation g_face_micro_anims[KERFUR_FACE_MICRO_ANIM_COUNT] = {{
{chr(10).join(micro_lines)}
}};

static const struct kerfur_face_reaction g_face_reactions[REACTION_COUNT] = {{
{chr(10).join(reaction_lines)}
}};

const struct kerfur_face_recipe *kerfur_face_recipe_get(enum kerfur_face_recipe_id id)
{{
\tif ((id < 0) || (id >= KERFUR_FACE_RECIPE_COUNT)) {{
\t\treturn &g_face_recipes[KERFUR_FACE_RECIPE_PET_EXPR_CALM];
\t}}
\treturn &g_face_recipes[id];
}}

const struct kerfur_face_reaction *kerfur_face_reaction_get(enum kerfur_face_reaction_id id)
{{
\tif ((id < 0) || (id >= KERFUR_FACE_REACTION_REACTION_COUNT)) {{
\t\treturn &g_face_reactions[KERFUR_FACE_REACTION_REACTION_NONE];
\t}}
\treturn &g_face_reactions[id];
}}

const struct kerfur_face_micro_animation *kerfur_face_micro_anim_get(enum kerfur_face_micro_anim_id id)
{{
\tif ((id <= KERFUR_FACE_MICRO_ANIM_NONE) || (id >= KERFUR_FACE_MICRO_ANIM_COUNT)) {{
\t\treturn &g_face_micro_anims[KERFUR_FACE_MICRO_ANIM_NONE];
\t}}
\treturn &g_face_micro_anims[id];
}}

const char *kerfur_face_recipe_name(enum kerfur_face_recipe_id id)
{{
\treturn kerfur_face_recipe_get(id)->name;
}}

const char *kerfur_face_reaction_name(enum kerfur_face_reaction_id id)
{{
\treturn kerfur_face_reaction_get(id)->name;
}}
"""


def parse_args() -> argparse.Namespace:
	parser = argparse.ArgumentParser(description="Generate Kerfur face assets and recipe tables.")
	parser.add_argument("--input", type=Path, default=DEFAULT_INPUT)
	parser.add_argument("--out-include", type=Path, default=DEFAULT_OUT_INCLUDE)
	parser.add_argument("--out-src", type=Path, default=DEFAULT_OUT_SRC)
	parser.add_argument("--check", action="store_true")
	parser.add_argument("--verbose", action="store_true")
	return parser.parse_args()


def main() -> int:
	args = parse_args()
	generator = FaceCodegen(args.input)
	outputs = generator.generate()

	remapped_outputs: dict[Path, str] = {}
	for path, content in outputs.items():
		if path.parent == DEFAULT_OUT_INCLUDE:
			remapped_outputs[args.out_include / path.name] = content
		elif path.parent == DEFAULT_OUT_SRC:
			remapped_outputs[args.out_src / path.name] = content
		else:
			remapped_outputs[path] = content

	if args.check:
		for path, content in remapped_outputs.items():
			if not path.exists() or path.read_text(encoding="utf-8") != content:
				sys.stderr.write(f"{path} is out of date\n")
				return 1
	else:
		for path, content in remapped_outputs.items():
			write_if_changed(path, content)

	for warning in generator.warnings:
		sys.stderr.write(f"warning: {warning}\n")
	if args.verbose:
		sys.stdout.write(f"Generated {len(remapped_outputs)} files from {args.input}\n")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
