const JSON_URL = "../face/kerfur_faces.json";
const ASSET_ROOT = "../face/";
const EXPRESSION_REFERENCE_ROOT = "../face/references/expressions/";
const REACTION_REFERENCE_ROOT = "../face/references/reactions/";

const REACTION_SLOT_FIELDS = {
  temporary_left_eye_white: "left_eye_white",
  temporary_right_eye_white: "right_eye_white",
  temporary_left_eyeball: "left_eyeball",
  temporary_right_eyeball: "right_eyeball",
  temporary_indicator: "indicator",
  temporary_overlay: "overlay",
};

const VISUALIZER_BASES = {
  whisker_left_x: 0,
  whisker_y: 46,
};

const SAMPLE_BATTERY_PERCENT = "73%";

const elements = {
  expressionSelect: document.getElementById("expression-select"),
  reactionSelect: document.getElementById("reaction-select"),
  scaleRange: document.getElementById("scale-range"),
  aliasesToggle: document.getElementById("aliases-toggle"),
  gridToggle: document.getElementById("grid-toggle"),
  reloadButton: document.getElementById("reload-button"),
  statusOutput: document.getElementById("status-output"),
  warningList: document.getElementById("warning-list"),
  resolvedList: document.getElementById("resolved-list"),
  generatedShell: document.getElementById("generated-shell"),
  generatedStage: document.getElementById("generated-stage"),
  expressionReferenceShell: document.getElementById("expression-reference-shell"),
  expressionReferenceImage: document.getElementById("expression-reference-image"),
  expressionReferenceCaption: document.getElementById("expression-reference-caption"),
  reactionReferenceCard: document.getElementById("reaction-reference-card"),
  reactionReferenceShell: document.getElementById("reaction-reference-shell"),
  reactionReferenceImage: document.getElementById("reaction-reference-image"),
  reactionReferenceCaption: document.getElementById("reaction-reference-caption"),
};

const state = {
  model: null,
  assets: {},
  expressions: new Map(),
  reactions: new Map(),
  scale: Number(elements.scaleRange.value),
};

function clamp(value, min, max) {
  return Math.min(Math.max(value, min), max);
}

function encodePath(path) {
  return encodeURI(path);
}

function getCanvas() {
  return state.model?.defaults?.canvas ?? { width: 128, height: 64 };
}

function getAnchors() {
  return state.model?.defaults?.anchors ?? {};
}

function clearChildren(node) {
  while (node.firstChild) {
    node.removeChild(node.firstChild);
  }
}

function buildIndex(items) {
  const map = new Map();
  for (const item of items ?? []) {
    if (item?.id) {
      map.set(item.id, item);
    }
  }
  return map;
}

function hasOwn(object, key) {
  return object != null && Object.prototype.hasOwnProperty.call(object, key);
}

function mergeOverride(baseOverride, reactionOverride) {
  const merged = {};
  const base = baseOverride ?? {};
  const extra = reactionOverride ?? {};

  if (base.x !== undefined) {
    merged.x = base.x;
  }
  if (base.y !== undefined) {
    merged.y = base.y;
  }
  if (extra.x !== undefined) {
    merged.x = extra.x;
  }
  if (extra.y !== undefined) {
    merged.y = extra.y;
  }

  merged.dx = Number(base.dx ?? 0) + Number(extra.dx ?? 0);
  merged.dy = Number(base.dy ?? 0) + Number(extra.dy ?? 0);
  return merged;
}

function applyOverride(position, override) {
  const resolved = { ...position };
  if (!override) {
    return resolved;
  }

  if (override.x !== undefined) {
    resolved.x = Number(override.x);
  }
  if (override.y !== undefined) {
    resolved.y = Number(override.y);
  }

  resolved.x += Number(override.dx ?? 0);
  resolved.y += Number(override.dy ?? 0);
  return resolved;
}

function effectiveSlotOverrides(expression, reaction, slotKey) {
  return mergeOverride(
    expression?.slot_overrides?.[slotKey],
    reaction?.slot_overrides?.[slotKey],
  );
}

function reactionComposeMode(reaction) {
  if (!reaction) {
    return "overlay";
  }

  return reaction.compose_mode ?? (reaction.base_expression ? "rebase" : "overlay");
}

function reactionSlotEntry(reaction, slotKey) {
  if (!reaction) {
    return { defined: false, value: null };
  }

  if (hasOwn(reaction, slotKey)) {
    return { defined: true, value: reaction[slotKey] };
  }

  if (hasOwn(reaction.face_slots, slotKey)) {
    return { defined: true, value: reaction.face_slots[slotKey] };
  }

  for (const [field, mappedSlot] of Object.entries(REACTION_SLOT_FIELDS)) {
    if ((mappedSlot === slotKey) && hasOwn(reaction, field)) {
      return { defined: true, value: reaction[field] };
    }
  }

  return { defined: false, value: null };
}

function effectiveReactionSlot(baseValue, reaction, slotKey) {
  const entry = reactionSlotEntry(reaction, slotKey);
  return entry.defined ? entry.value : baseValue;
}

function effectiveReactionValue(baseValue, reaction, key) {
  if (!reaction || !hasOwn(reaction, key)) {
    return baseValue;
  }

  return reaction[key];
}

function resolveReactionBaseExpression(selectedExpression, reaction, warnings) {
  if (!reaction) {
    return selectedExpression;
  }

  if (reactionComposeMode(reaction) !== "rebase") {
    return selectedExpression;
  }

  if (!reaction.base_expression) {
    warnings.push(`Reaction ${reaction.id} uses compose_mode "rebase" without base_expression.`);
    return selectedExpression;
  }

  const baseExpression = state.expressions.get(reaction.base_expression);
  if (!baseExpression) {
    warnings.push(`Missing base expression "${reaction.base_expression}" for reaction ${reaction.id}.`);
    return selectedExpression;
  }

  return baseExpression;
}

function collectEffectInstances(expression, reaction) {
  const instances = [];

  for (const effectId of expression.default_effects ?? []) {
    if (effectId) {
      instances.push({ effectId });
    }
  }

  if (reaction?.temporary_effect) {
    instances.push({ effectId: reaction.temporary_effect });
  }

  for (const effect of reaction?.temporary_effects ?? []) {
    if (typeof effect === "string") {
      instances.push({ effectId: effect });
      continue;
    }

    const effectId = effect?.effect_id ?? effect?.id;
    if (!effectId) {
      continue;
    }

    instances.push({
      effectId,
      x: effect.x,
      y: effect.y,
      dx: effect.dx,
      dy: effect.dy,
    });
  }

  return instances;
}

function resolveLegacyAlias(groupName, assetId, slotKey) {
  if (!elements.aliasesToggle.checked || !assetId) {
    return null;
  }

  const side = slotKey.startsWith("left_") ? "left" : slotKey.startsWith("right_") ? "right" : null;

  if (groupName === "pupils") {
    const sided = {
      pupil_round: {
        left: { id: "pupil_round_left" },
        right: { id: "pupil_round_right" },
      },
      pupil_glossy: {
        left: { id: "pupil_glossy_left" },
        right: { id: "pupil_glossy_right" },
      },
      pupil_playful: {
        left: { id: "pupil_playful_left" },
        right: { id: "pupil_playful_right" },
      },
      pupil_cozy: {
        left: { id: "pupil_cozy_left" },
        right: { id: "pupil_cozy_right" },
      },
      pupil_disconnect: {
        left: { id: "pupil_disconnect_left" },
        right: { id: "pupil_disconnect_right" },
      },
      pupil_charge: {
        left: { id: "pupil_charge_left" },
        right: { id: "pupil_charge_right" },
      },
      pupil_low_batt: {
        left: { id: "pupil_low_batt_left" },
        right: { id: "pupil_low_batt_left", mirror: true },
      },
    };

    if (sided[assetId] && side) {
      return sided[assetId][side];
    }
  }

  if (groupName === "eye_whites") {
    const aliases = {
      eye_white_sleepy_flat: { id: "eye_white_half_soft" },
      eye_white_wide: { id: "eye_white_open_round" },
    };
    return aliases[assetId] ?? null;
  }

  if (groupName === "eyebrows") {
    const aliases = {
      brow_sleepy: { id: "brow_soft" },
    };
    return aliases[assetId] ?? null;
  }

  if (groupName === "mouths") {
    const aliases = {
      mouth_cat_smile_small: { id: "mouth_default" },
      mouth_cat_open_happy: { id: "mouth_happy_open" },
      mouth_flat: { id: "mouth_annoyed" },
      mouth_sleepy: { id: "mouth_default" },
    };
    return aliases[assetId] ?? null;
  }

  if (groupName === "effects") {
    const aliases = {
      effect_tear_small: { id: "effect_tear" },
    };
    return aliases[assetId] ?? null;
  }

  return null;
}

function resolveAsset(groupName, assetId, slotKey, warnings, aliases) {
  if (!assetId) {
    return null;
  }

  const group = state.assets[groupName] ?? new Map();
  if (group.has(assetId)) {
    return {
      asset: group.get(assetId),
      resolvedId: assetId,
      mirror: false,
    };
  }

  const alias = resolveLegacyAlias(groupName, assetId, slotKey);
  if (alias && group.has(alias.id)) {
    aliases.push(`${slotKey}: ${assetId} -> ${alias.id}`);
    return {
      asset: group.get(alias.id),
      resolvedId: alias.id,
      mirror: Boolean(alias.mirror),
    };
  }

  warnings.push(`Missing ${groupName} asset "${assetId}" for ${slotKey}.`);
  return null;
}

function eyeWhitePosition(asset, side, override) {
  const anchors = getAnchors();
  const leftBaseX = Number(anchors.left_eye_x ?? 17);
  const rightBaseX = Number(anchors.right_eye_x ?? 74);
  const baseY = Number(side === "left" ? anchors.left_eye_y ?? 16 : anchors.right_eye_y ?? 16);
  const assetX = Number(asset?.anchor_x ?? (side === "left" ? leftBaseX : rightBaseX));
  const assetY = Number(asset?.anchor_y ?? baseY);
  const x = side === "left" ? assetX : rightBaseX + (assetX - leftBaseX);
  return applyOverride({ x, y: assetY }, override);
}

function slotBasePosition(slotKey, asset, override) {
  const anchors = getAnchors();
  const canvas = getCanvas();
  let base = { x: 0, y: 0 };

  switch (slotKey) {
    case "left_brow":
      base = {
        x: Number(anchors.brow_left_x ?? 46),
        y: Number(anchors.brow_left_y ?? 8),
      };
      break;
    case "right_brow":
      base = {
        x: Number(anchors.brow_right_x ?? 72),
        y: Number(anchors.brow_right_y ?? 8),
      };
      break;
    case "mouth":
      base = {
        x: Number(anchors.mouth_x ?? 54),
        y: Number(anchors.mouth_y ?? 38),
      };
      break;
    case "indicator":
      base = {
        x: Number(anchors.indicator_x ?? 3),
        y: Number(anchors.indicator_y ?? 3),
      };
      break;
    case "overlay":
      base = {
        x: Number(anchors.overlay_x ?? 3),
        y: Number(anchors.overlay_y ?? 3),
      };
      break;
    case "effect":
      base = {
        x: Number(anchors.effect_x ?? 104),
        y: Number(anchors.effect_y ?? 6),
      };
      break;
    case "left_whisker":
      base = {
        x: VISUALIZER_BASES.whisker_left_x,
        y: VISUALIZER_BASES.whisker_y,
      };
      break;
    case "right_whisker":
      base = {
        x: canvas.width - 1 - Number(asset?.width ?? 0),
        y: VISUALIZER_BASES.whisker_y,
      };
      break;
    default:
      base = { x: 0, y: 0 };
      break;
  }

  return applyOverride(base, override);
}

function buildSvgUrl(file) {
  return encodePath(`${ASSET_ROOT}${file}`);
}

function buildReferenceUrl(type, id) {
  const root = type === "expression" ? EXPRESSION_REFERENCE_ROOT : REACTION_REFERENCE_ROOT;
  return encodePath(`${root}- ${id}.svg`);
}

function computeLook(expression, reaction, eyeWhiteAsset) {
  const baseLook = expression?.base_look ?? { x: 0, y: 0 };
  const reactionLook = reaction?.look_offset ?? { x: 0, y: 0 };
  const combined = {
    x: clamp(Number(baseLook.x ?? 0) + Number(reactionLook.x ?? 0), -100, 100),
    y: clamp(Number(baseLook.y ?? 0) + Number(reactionLook.y ?? 0), -100, 100),
  };

  if (!expression?.allow_dynamic_pupils || !eyeWhiteAsset?.eye_solver) {
    return {
      raw: combined,
      dx: 0,
      dy: 0,
    };
  }

  return {
    raw: combined,
    dx: Math.round((combined.x / 100) * Number(eyeWhiteAsset.eye_solver.rx ?? 0)),
    dy: Math.round((combined.y / 100) * Number(eyeWhiteAsset.eye_solver.ry ?? 0)),
  };
}

function effectPosition(effectAsset, effectId, eyeRects, index) {
  const base = slotBasePosition("effect", effectAsset, null);
  const leftEye = eyeRects.left;
  const anchorX = Number(effectAsset?.anchor_x ?? 0);
  const anchorY = Number(effectAsset?.anchor_y ?? 0);

  if (effectId.includes("tear")) {
    if (leftEye) {
      return {
        x: leftEye.x + leftEye.width - 4 - anchorX,
        y: leftEye.y + leftEye.height + 1 - anchorY,
      };
    }
  }

  return {
    x: base.x + (index * 4),
    y: base.y + (index * 2),
  };
}

function createLayerNode(layer) {
  const node = document.createElement("div");
  node.className = "asset-layer";
  node.style.left = `${layer.x}px`;
  node.style.top = `${layer.y}px`;
  node.style.width = `${layer.width}px`;
  node.style.height = `${layer.height}px`;

  const image = document.createElement("img");
  image.className = "asset-image";
  if (layer.mirror) {
    image.classList.add("mirrored");
  }
  image.src = layer.url;
  image.alt = layer.label;
  image.draggable = false;
  node.title = layer.label;
  node.appendChild(image);
  return node;
}

function createEyeLayerNode(eyeLayer) {
  const node = document.createElement("div");
  node.className = "eye-layer";
  node.style.left = `${eyeLayer.x}px`;
  node.style.top = `${eyeLayer.y}px`;
  node.style.width = `${eyeLayer.width}px`;
  node.style.height = `${eyeLayer.height}px`;
  node.title = eyeLayer.label;

  const eyeImage = document.createElement("img");
  eyeImage.className = "asset-image";
  if (eyeLayer.mirrorEye) {
    eyeImage.classList.add("mirrored");
  }
  eyeImage.src = eyeLayer.eyeUrl;
  eyeImage.alt = eyeLayer.label;
  eyeImage.draggable = false;
  node.appendChild(eyeImage);

  if (eyeLayer.pupilUrl) {
    const clip = document.createElement("div");
    clip.className = "pupil-clip";

    const pupil = document.createElement("img");
    pupil.className = "pupil-image";
    if (eyeLayer.mirrorPupil) {
      pupil.classList.add("mirrored");
    }
    pupil.src = eyeLayer.pupilUrl;
    pupil.alt = `${eyeLayer.label} pupil`;
    pupil.draggable = false;
    pupil.style.left = `${eyeLayer.pupilOffsetX}px`;
    pupil.style.top = `${eyeLayer.pupilOffsetY}px`;
    pupil.style.width = `${eyeLayer.pupilWidth}px`;
    pupil.style.height = `${eyeLayer.pupilHeight}px`;
    clip.appendChild(pupil);
    node.appendChild(clip);
  }

  return node;
}

function createOverlayTextNode(position, text) {
  const node = document.createElement("div");
  node.className = "overlay-text";
  node.style.left = `${position.x}px`;
  node.style.top = `${position.y}px`;
  node.textContent = text;
  return node;
}

function collectResolvedEntry(lines, label, id, position, extra = "") {
  const suffix = extra ? ` ${extra}` : "";
  lines.push(`${label}: ${id} @ ${position.x}, ${position.y}${suffix}`);
}

function resolveComposition(expression, reaction) {
  const warnings = [];
  const aliases = [];
  const resolved = [];
  const layers = [];
  const baseExpression = resolveReactionBaseExpression(expression, reaction, warnings);
  const composeMode = reactionComposeMode(reaction);

  const effective = {
    id: baseExpression.id,
    base_look: effectiveReactionValue(baseExpression.base_look ?? { x: 0, y: 0 }, reaction, "base_look"),
    allow_dynamic_pupils: Boolean(
      effectiveReactionValue(baseExpression.allow_dynamic_pupils ?? true, reaction, "allow_dynamic_pupils"),
    ),
    left_eye_white: effectiveReactionSlot(baseExpression.left_eye_white, reaction, "left_eye_white"),
    right_eye_white: effectiveReactionSlot(baseExpression.right_eye_white, reaction, "right_eye_white"),
    left_eyeball: effectiveReactionSlot(baseExpression.left_eyeball, reaction, "left_eyeball"),
    right_eyeball: effectiveReactionSlot(baseExpression.right_eyeball, reaction, "right_eyeball"),
    left_brow: effectiveReactionSlot(baseExpression.left_brow, reaction, "left_brow"),
    right_brow: effectiveReactionSlot(baseExpression.right_brow, reaction, "right_brow"),
    mouth: effectiveReactionSlot(baseExpression.mouth, reaction, "mouth"),
    whiskers: effectiveReactionSlot(baseExpression.whiskers, reaction, "whiskers"),
    indicator: effectiveReactionSlot(baseExpression.indicator ?? null, reaction, "indicator"),
    overlay: effectiveReactionSlot(baseExpression.overlay ?? null, reaction, "overlay"),
    default_effects: effectiveReactionValue(baseExpression.default_effects ?? [], reaction, "default_effects"),
  };

  const leftEyeWhite = resolveAsset("eye_whites", effective.left_eye_white, "left_eye_white", warnings, aliases);
  const rightEyeWhite = resolveAsset("eye_whites", effective.right_eye_white, "right_eye_white", warnings, aliases);
  const leftPupil = resolveAsset("pupils", effective.left_eyeball, "left_eyeball", warnings, aliases);
  const rightPupil = resolveAsset("pupils", effective.right_eyeball, "right_eyeball", warnings, aliases);
  const leftBrow = resolveAsset("eyebrows", effective.left_brow, "left_brow", warnings, aliases);
  const rightBrow = resolveAsset("eyebrows", effective.right_brow, "right_brow", warnings, aliases);
  const mouth = resolveAsset("mouths", effective.mouth, "mouth", warnings, aliases);
  const whiskers = resolveAsset("whiskers", effective.whiskers, "whiskers", warnings, aliases);
  const indicator = resolveAsset("indicators", effective.indicator, "indicator", warnings, aliases);
  const overlay = resolveAsset("overlays", effective.overlay, "overlay", warnings, aliases);

  const look = computeLook(effective, reaction, leftEyeWhite?.asset);
  const eyeRects = {};

  if (leftEyeWhite) {
    const override = effectiveSlotOverrides(baseExpression, reaction, "left_eye_white");
    const position = eyeWhitePosition(leftEyeWhite.asset, "left", override);
    eyeRects.left = {
      x: position.x,
      y: position.y,
      width: Number(leftEyeWhite.asset.width),
      height: Number(leftEyeWhite.asset.height),
    };

    const pupilOverride = effectiveSlotOverrides(baseExpression, reaction, "left_eyeball");
    const pupilPosition = leftPupil
      ? applyOverride(
          {
            x: position.x + Number(leftPupil.asset.anchor_x ?? 0) + look.dx,
            y: position.y + Number(leftPupil.asset.anchor_y ?? 0) + look.dy,
          },
          pupilOverride,
        )
      : null;

    layers.push({
      type: "eye",
      x: position.x,
      y: position.y,
      width: Number(leftEyeWhite.asset.width),
      height: Number(leftEyeWhite.asset.height),
      eyeUrl: buildSvgUrl(leftEyeWhite.asset.file),
      pupilUrl: leftPupil ? buildSvgUrl(leftPupil.asset.file) : null,
      pupilOffsetX: pupilPosition ? pupilPosition.x - position.x : 0,
      pupilOffsetY: pupilPosition ? pupilPosition.y - position.y : 0,
      pupilWidth: leftPupil ? Number(leftPupil.asset.width) : 0,
      pupilHeight: leftPupil ? Number(leftPupil.asset.height) : 0,
      mirrorEye: false,
      mirrorPupil: Boolean(leftPupil?.mirror),
      label: "left eye",
    });

    collectResolvedEntry(resolved, "left_eye_white", leftEyeWhite.resolvedId, position);
    if (leftPupil && pupilPosition) {
      collectResolvedEntry(resolved, "left_eyeball", leftPupil.resolvedId, pupilPosition, `[look ${look.dx},${look.dy}]`);
    }
  }

  if (rightEyeWhite) {
    const override = effectiveSlotOverrides(baseExpression, reaction, "right_eye_white");
    const position = eyeWhitePosition(rightEyeWhite.asset, "right", override);
    eyeRects.right = {
      x: position.x,
      y: position.y,
      width: Number(rightEyeWhite.asset.width),
      height: Number(rightEyeWhite.asset.height),
    };

    const pupilOverride = effectiveSlotOverrides(baseExpression, reaction, "right_eyeball");
    const pupilPosition = rightPupil
      ? applyOverride(
          {
            x: position.x + Number(rightPupil.asset.anchor_x ?? 0) + look.dx,
            y: position.y + Number(rightPupil.asset.anchor_y ?? 0) + look.dy,
          },
          pupilOverride,
        )
      : null;

    layers.push({
      type: "eye",
      x: position.x,
      y: position.y,
      width: Number(rightEyeWhite.asset.width),
      height: Number(rightEyeWhite.asset.height),
      eyeUrl: buildSvgUrl(rightEyeWhite.asset.file),
      pupilUrl: rightPupil ? buildSvgUrl(rightPupil.asset.file) : null,
      pupilOffsetX: pupilPosition ? pupilPosition.x - position.x : 0,
      pupilOffsetY: pupilPosition ? pupilPosition.y - position.y : 0,
      pupilWidth: rightPupil ? Number(rightPupil.asset.width) : 0,
      pupilHeight: rightPupil ? Number(rightPupil.asset.height) : 0,
      mirrorEye: Boolean(rightEyeWhite.asset.mirrorable),
      mirrorPupil: Boolean(rightPupil?.mirror),
      label: "right eye",
    });

    collectResolvedEntry(resolved, "right_eye_white", rightEyeWhite.resolvedId, position);
    if (rightPupil && pupilPosition) {
      collectResolvedEntry(resolved, "right_eyeball", rightPupil.resolvedId, pupilPosition, `[look ${look.dx},${look.dy}]`);
    }
  }

  if (whiskers) {
    const whiskerOverride = effectiveSlotOverrides(baseExpression, reaction, "whiskers");
    const leftPosition = slotBasePosition("left_whisker", whiskers.asset, whiskerOverride);
    const rightPosition = slotBasePosition("right_whisker", whiskers.asset, whiskerOverride);
    const mirrorRightWhisker = Boolean(whiskers.asset.mirror_right);

    layers.push({
      type: "asset",
      x: leftPosition.x,
      y: leftPosition.y,
      width: Number(whiskers.asset.width),
      height: Number(whiskers.asset.height),
      url: buildSvgUrl(whiskers.asset.file),
      mirror: false,
      label: "left whisker",
    });
    layers.push({
      type: "asset",
      x: rightPosition.x,
      y: rightPosition.y,
      width: Number(whiskers.asset.width),
      height: Number(whiskers.asset.height),
      url: buildSvgUrl(whiskers.asset.file),
      mirror: mirrorRightWhisker,
      label: "right whisker",
    });

    collectResolvedEntry(resolved, "whiskers_left", whiskers.resolvedId, leftPosition);
    collectResolvedEntry(
      resolved,
      "whiskers_right",
      whiskers.resolvedId,
      rightPosition,
      mirrorRightWhisker ? "[mirrored]" : "",
    );
  }

  if (mouth) {
    const position = slotBasePosition(
      "mouth",
      mouth.asset,
      effectiveSlotOverrides(baseExpression, reaction, "mouth"),
    );
    layers.push({
      type: "asset",
      x: position.x,
      y: position.y,
      width: Number(mouth.asset.width),
      height: Number(mouth.asset.height),
      url: buildSvgUrl(mouth.asset.file),
      mirror: false,
      label: "mouth",
    });
    collectResolvedEntry(resolved, "mouth", mouth.resolvedId, position);
  }

  if (leftBrow) {
    const position = slotBasePosition(
      "left_brow",
      leftBrow.asset,
      effectiveSlotOverrides(baseExpression, reaction, "left_brow"),
    );
    layers.push({
      type: "asset",
      x: position.x,
      y: position.y,
      width: Number(leftBrow.asset.width),
      height: Number(leftBrow.asset.height),
      url: buildSvgUrl(leftBrow.asset.file),
      mirror: false,
      label: "left brow",
    });
    collectResolvedEntry(resolved, "left_brow", leftBrow.resolvedId, position);
  }

  if (rightBrow) {
    const position = slotBasePosition(
      "right_brow",
      rightBrow.asset,
      effectiveSlotOverrides(baseExpression, reaction, "right_brow"),
    );
    layers.push({
      type: "asset",
      x: position.x,
      y: position.y,
      width: Number(rightBrow.asset.width),
      height: Number(rightBrow.asset.height),
      url: buildSvgUrl(rightBrow.asset.file),
      mirror: Boolean(rightBrow.asset.mirrorable),
      label: "right brow",
    });
    collectResolvedEntry(resolved, "right_brow", rightBrow.resolvedId, position);
  }

  if (indicator) {
    const position = slotBasePosition(
      "indicator",
      indicator.asset,
      effectiveSlotOverrides(baseExpression, reaction, "indicator"),
    );
    layers.push({
      type: "asset",
      x: position.x,
      y: position.y,
      width: Number(indicator.asset.width),
      height: Number(indicator.asset.height),
      url: buildSvgUrl(indicator.asset.file),
      mirror: false,
      label: "indicator",
    });
    collectResolvedEntry(resolved, "indicator", indicator.resolvedId, position);
  }

  if (overlay?.asset?.render_mode === "text_runtime" || effective.overlay === "overlay_battery_percent") {
    const position = slotBasePosition(
      "overlay",
      overlay?.asset,
      effectiveSlotOverrides(baseExpression, reaction, "overlay"),
    );
    layers.push({
      type: "overlay_text",
      x: position.x,
      y: position.y,
      text: SAMPLE_BATTERY_PERCENT,
    });
    collectResolvedEntry(resolved, "overlay", effective.overlay ?? "overlay_battery_percent", position, `[text ${SAMPLE_BATTERY_PERCENT}]`);
  }

  const effectInstances = collectEffectInstances(effective, reaction);

  effectInstances.forEach((effectInstance, index) => {
    const resolvedEffect = resolveAsset("effects", effectInstance.effectId, `effect_${index + 1}`, warnings, aliases);
    if (!resolvedEffect) {
      return;
    }

    const position = (effectInstance.x !== undefined || effectInstance.y !== undefined)
      ? applyOverride(
          {
            x: Number(effectInstance.x ?? 0),
            y: Number(effectInstance.y ?? 0),
          },
          {
            dx: Number(effectInstance.dx ?? 0),
            dy: Number(effectInstance.dy ?? 0),
          },
        )
      : applyOverride(
          effectPosition(resolvedEffect.asset, resolvedEffect.resolvedId, eyeRects, index),
          effectiveSlotOverrides(baseExpression, reaction, "effect"),
        );

    layers.push({
      type: "asset",
      x: position.x,
      y: position.y,
      width: Number(resolvedEffect.asset.width),
      height: Number(resolvedEffect.asset.height),
      url: buildSvgUrl(resolvedEffect.asset.file),
      mirror: false,
      label: `effect ${resolvedEffect.resolvedId}`,
    });
    collectResolvedEntry(resolved, `effect_${index + 1}`, resolvedEffect.resolvedId, position);
  });

  return {
    warnings,
    aliases,
    resolved,
    layers,
    baseExpressionId: baseExpression.id,
    composeMode,
  };
}

function fillList(node, items) {
  clearChildren(node);

  if (items.length === 0) {
    const li = document.createElement("li");
    li.textContent = "None.";
    node.appendChild(li);
    return;
  }

  for (const item of items) {
    const li = document.createElement("li");
    li.textContent = item;
    node.appendChild(li);
  }
}

function renderGenerated(expression, reaction) {
  const canvas = getCanvas();
  const composition = resolveComposition(expression, reaction);

  clearChildren(elements.generatedStage);
  elements.generatedStage.style.width = `${canvas.width}px`;
  elements.generatedStage.style.height = `${canvas.height}px`;
  elements.generatedStage.style.transform = `scale(${state.scale})`;
  elements.generatedStage.classList.toggle("grid", elements.gridToggle.checked);
  elements.generatedShell.style.minHeight = `${canvas.height * state.scale + 32}px`;

  for (const layer of composition.layers) {
    if (layer.type === "eye") {
      elements.generatedStage.appendChild(createEyeLayerNode(layer));
    } else if (layer.type === "overlay_text") {
      elements.generatedStage.appendChild(createOverlayTextNode(layer, layer.text));
    } else {
      elements.generatedStage.appendChild(createLayerNode(layer));
    }
  }

  const warningItems = [];
  for (const alias of composition.aliases) {
    warningItems.push(`Alias: ${alias}`);
  }
  for (const warning of composition.warnings) {
    warningItems.push(warning);
  }

  fillList(elements.warningList, warningItems);
  fillList(elements.resolvedList, composition.resolved);

  elements.statusOutput.textContent =
    `Canvas ${canvas.width}x${canvas.height}. ` +
    `${Object.values(state.assets).reduce((sum, map) => sum + map.size, 0)} catalogued assets. ` +
    `Expression ${expression.id}. ` +
    `Effective ${composition.baseExpressionId}. ` +
    `Reaction ${reaction?.id ?? "REACTION_NONE"} [${composition.composeMode}].`;

  return composition;
}

function renderReference(imageElement, shellElement, captionElement, url, caption, scale) {
  captionElement.textContent = caption;
  imageElement.src = url;
  imageElement.alt = caption;
  imageElement.style.transform = `scale(${scale})`;
  imageElement.style.width = "128px";
  imageElement.style.height = "64px";
  shellElement.style.minHeight = `${64 * scale + 32}px`;
}

function render() {
  if (!state.model) {
    return;
  }

  const expression = state.expressions.get(elements.expressionSelect.value) ?? Array.from(state.expressions.values())[0];
  const reaction =
    elements.reactionSelect.value === "REACTION_NONE"
      ? null
      : state.reactions.get(elements.reactionSelect.value) ?? null;

  const composition = renderGenerated(expression, reaction);
  const referenceExpression =
    state.expressions.get(composition.baseExpressionId) ??
    expression;
  const expressionCaption =
    referenceExpression.id === expression.id
      ? expression.id
      : `${expression.id} -> ${referenceExpression.id} [${composition.composeMode}]`;

  renderReference(
    elements.expressionReferenceImage,
    elements.expressionReferenceShell,
    elements.expressionReferenceCaption,
    buildReferenceUrl("expression", referenceExpression.id),
    expressionCaption,
    state.scale,
  );

  if (reaction && reaction.id !== "REACTION_NONE") {
    elements.reactionReferenceCard.hidden = false;
    renderReference(
      elements.reactionReferenceImage,
      elements.reactionReferenceShell,
      elements.reactionReferenceCaption,
      buildReferenceUrl("reaction", reaction.id),
      reaction.id,
      state.scale,
    );
  } else {
    elements.reactionReferenceCard.hidden = true;
  }
}

function populateSelect(select, values, selectedId) {
  clearChildren(select);
  for (const value of values) {
    const option = document.createElement("option");
    option.value = value.id;
    option.textContent = value.id;
    if (value.id === selectedId) {
      option.selected = true;
    }
    select.appendChild(option);
  }
}

async function loadModel() {
  const response = await fetch(`${JSON_URL}?t=${Date.now()}`, { cache: "no-store" });
  if (!response.ok) {
    throw new Error(`Failed to load ${JSON_URL}: ${response.status}`);
  }

  state.model = await response.json();
  state.assets = {};
  state.expressions = new Map();
  state.reactions = new Map();

  for (const [groupName, group] of Object.entries(state.model.asset_groups ?? {})) {
    state.assets[groupName] = buildIndex(group.items);
  }

  for (const expression of state.model.expressions ?? []) {
    state.expressions.set(expression.id, expression);
  }

  for (const reaction of state.model.reactions ?? []) {
    state.reactions.set(reaction.id, reaction);
  }

  const expressionValues = Array.from(state.expressions.values());
  const reactionValues = Array.from(state.reactions.values());
  const reactionOptions = reactionValues.some((reaction) => reaction.id === "REACTION_NONE")
    ? reactionValues
    : [{ id: "REACTION_NONE" }, ...reactionValues];

  populateSelect(elements.expressionSelect, expressionValues, expressionValues[0]?.id ?? "");
  populateSelect(elements.reactionSelect, reactionOptions, "REACTION_NONE");
}

async function reloadAndRender() {
  try {
    elements.statusOutput.textContent = "Loading kerfur_faces.json...";
    await loadModel();
    render();
  } catch (error) {
    elements.statusOutput.textContent = String(error);
    fillList(elements.warningList, [String(error)]);
    fillList(elements.resolvedList, []);
  }
}

elements.expressionSelect.addEventListener("change", render);
elements.reactionSelect.addEventListener("change", render);
elements.scaleRange.addEventListener("input", () => {
  state.scale = Number(elements.scaleRange.value);
  render();
});
elements.aliasesToggle.addEventListener("change", render);
elements.gridToggle.addEventListener("change", render);
elements.reloadButton.addEventListener("click", reloadAndRender);

reloadAndRender();
