#pragma once

namespace SnowShapes
{
	bool GetShapeBound(RE::bhkNiCollisionObject* collisionObj, RE::NiPoint3& centerPos, float& radius);

	bool ExtractShapeHalfExtents(const RE::hkpShape* shape, float& hx, float& hy, float& hz);

	bool GetShapeFootprint(RE::bhkNiCollisionObject* collisionObj, float& axisX, float& axisY, float& halfLength, float& halfWidth, float& halfHeight);
}
