#include "ActorUtils.h"
#include <algorithm>
#include <cmath>

namespace Util
{
	bool GetShapeBound(RE::bhkNiCollisionObject* collisionObj, RE::NiPoint3& centerPos, float& radius)
	{
		if (!collisionObj)
			return false;

		RE::bhkRigidBody* bhkRigid = collisionObj->body.get() ? collisionObj->body.get()->AsBhkRigidBody() : nullptr;
		RE::hkpRigidBody* hkpRigid = bhkRigid ? skyrim_cast<RE::hkpRigidBody*>(bhkRigid->referencedObject.get()) : nullptr;
		if (bhkRigid && hkpRigid && !skyrim_cast<RE::hkpListShape*>(hkpRigid)) {  // Ignore hkpListShape, unsupported
			RE::hkVector4 massCenter;
			bhkRigid->GetCenterOfMassWorld(massCenter);
			float massTrans[4];
			// Use unaligned store to avoid UB from potential stack misalignment
			_mm_storeu_ps(massTrans, massCenter.quad);
			centerPos = RE::NiPoint3(massTrans[0], massTrans[1], massTrans[2]) * RE::bhkWorld::GetWorldScaleInverse();
			return Util::ExtractShapeBound(hkpRigid->collidable.GetShape(), radius);
		}
		return false;
	}

	bool ExtractShapeHalfExtents(const RE::hkpShape* shape, float& hx, float& hy, float& hz)
	{
		if (!shape)
			return false;
		// Support mapping along each local axis, both ways, so the extents are
		// offset-invariant whatever the shape's origin.
		auto project = [shape](float x, float y, float z) {
			return shape->GetMaximumProjection(RE::hkVector4{ x, y, z, 0.0f }) * RE::bhkWorld::GetWorldScaleInverse();
		};
		hx = 0.5f * (project(1.0f, 0.0f, 0.0f) - project(-1.0f, 0.0f, 0.0f));
		hy = 0.5f * (project(0.0f, 1.0f, 0.0f) - project(0.0f, -1.0f, 0.0f));
		hz = 0.5f * (project(0.0f, 0.0f, 1.0f) - project(0.0f, 0.0f, -1.0f));
		return true;
	}

	bool GetShapeFootprint(RE::bhkNiCollisionObject* collisionObj, float& axisX, float& axisY, float& halfLength, float& halfWidth)
	{
		if (!collisionObj || !collisionObj->sceneObject)
			return false;
		RE::bhkRigidBody* bhkRigid = collisionObj->body.get() ? collisionObj->body.get()->AsBhkRigidBody() : nullptr;
		RE::hkpRigidBody* hkpRigid = bhkRigid ? skyrim_cast<RE::hkpRigidBody*>(bhkRigid->referencedObject.get()) : nullptr;
		if (!bhkRigid || !hkpRigid || skyrim_cast<RE::hkpListShape*>(hkpRigid))  // hkpListShape unsupported, as in GetShapeBound
			return false;
		float h[3];
		if (!ExtractShapeHalfExtents(hkpRigid->collidable.GetShape(), h[0], h[1], h[2]))
			return false;

		// Each local axis, scaled by its extent, rotated by the node Havok
		// drives, then flattened onto the ground.
		const auto& rot = collisionObj->sceneObject->world.rotate;
		const RE::NiPoint3 axes[3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } };
		float px[3], py[3], len[3];
		int longest = 0;
		for (int i = 0; i < 3; ++i) {
			const RE::NiPoint3 world = rot * axes[i];
			px[i] = world.x * h[i];
			py[i] = world.y * h[i];
			len[i] = sqrtf(px[i] * px[i] + py[i] * py[i]);
			if (len[i] > len[longest])
				longest = i;
		}
		halfLength = len[longest];
		halfWidth = 0.0f;
		for (int i = 0; i < 3; ++i)
			if (i != longest)
				halfWidth = std::max(halfWidth, len[i]);
		if (halfLength > 1e-3f) {
			axisX = px[longest] / halfLength;
			axisY = py[longest] / halfLength;
		} else {
			axisX = 1.0f;
			axisY = 0.0f;
		}
		return true;
	}

	bool ExtractShapeBound(const RE::hkpShape* shape, float& radius)
	{
		using ShapeType = RE::hkpShapeType;
		if (!shape)
			return false;

		auto symmetricHalfExtents = [shape](float& hx, float& hy, float& hz) {
			ExtractShapeHalfExtents(shape, hx, hy, hz);
		};
		auto halfDiagonal = [](float hx, float hy, float hz) {
			return sqrtf(hx * hx + hy * hy + hz * hz);
		};
		if (shape->type == ShapeType::kCapsule) {
			float hx, hy, hz;
			symmetricHalfExtents(hx, hy, hz);
			// For capsules, use the maximum half-extent (typically hz for vertical orientation)
			// as the farthest point lies along the capsule's main axis, not at the diagonal
			radius = std::max(hx, std::max(hy, hz));
			return true;
		} else if (shape->type == ShapeType::kSphere) {
			// For spheres, any axis should yield the same half-extent; use symmetric X
			float hx, hy, hz;
			symmetricHalfExtents(hx, hy, hz);
			radius = hx;
			return true;
		} else if (shape->type == ShapeType::kBox) {
			float hx, hy, hz;
			symmetricHalfExtents(hx, hy, hz);
			radius = halfDiagonal(hx, hy, hz);
			return true;
		} else if (shape->type == ShapeType::kCylinder) {
			// Use symmetric half-extents; cylinder radius is max of X/Y half-extents
			float hx, hy, hz;
			symmetricHalfExtents(hx, hy, hz);
			float hr = std::max(hx, hy);
			radius = sqrtf(hr * hr + hz * hz);
			return true;
		} else if (shape->type == ShapeType::kConvexVertices || shape->type == ShapeType::kTriangle) {
			// Offset-invariant estimate: take symmetric half-extents per axis and use the max
			float hx, hy, hz;
			symmetricHalfExtents(hx, hy, hz);
			radius = std::max(hx, std::max(hy, hz));
			return true;
		} else {
			// Fallback: mirror the convex/triangle approach for consistency
			float hx, hy, hz;
			symmetricHalfExtents(hx, hy, hz);
			radius = std::max(hx, std::max(hy, hz));
			return true;
		}
	}
}
