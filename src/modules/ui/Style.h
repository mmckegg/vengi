/**
 * @file
 */

#pragma once

#include "core/Color.h"

namespace style {

enum StyleColor { ColorReferenceNode, ColorInactiveNode, ColorActiveNode, ColorHighlightArea, ColorGridBorder };

const glm::vec4 &color(StyleColor color);

} // namespace style
