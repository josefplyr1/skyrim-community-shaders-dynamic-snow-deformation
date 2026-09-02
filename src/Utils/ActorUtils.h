

#pragma once
#include "RE/Skyrim.h"
#include <string>
#include <vector>

// Forward declarations for Skyrim types
namespace RE
{
	class bhkNiCollisionObject;
	class hkpShape;
	class NiPoint3;
}

namespace Util
{
	/**
     * @brief Extracts the shape bounds from a collision object.
     * @param collisionObj Pointer to the collision object.
     * @param centerPos Output: center position of the shape.
     * @param radius Output: radius of the shape.
     * @return True if bounds were successfully extracted, false otherwise.
     */
	bool GetShapeBound(RE::bhkNiCollisionObject* collisionObj, RE::NiPoint3& centerPos, float& radius);

	/**
     * @brief Extracts the shape bounds from a hkpShape.
     * @param shape Pointer to the shape.
     * @param radius Output: radius of the shape.
     * @return True if bounds were successfully extracted, false otherwise.
     */
	bool ExtractShapeBound(const RE::hkpShape* shape, float& radius);

	/**
     * @brief Half-extents of a hkpShape along its own local axes, in world units.
     * @param shape Pointer to the shape.
     * @param hx Output: half-extent along local X.
     * @param hy Output: half-extent along local Y.
     * @param hz Output: half-extent along local Z.
     * @return True if the shape could be measured, false otherwise.
     */
	bool ExtractShapeHalfExtents(const RE::hkpShape* shape, float& hx, float& hy, float& hz);

	/**
     * @brief The shape's silhouette on the ground: a segment along its longest horizontal extent, and the extent across it.
     *
     * The three local half-extents are rotated by the owning scene node and projected onto XY;
     * the longest projection is the segment, the longer of the other two the width. A sphere
     * reports equal length and width, so a caller subtracting one from the other gets a point.
     * @param collisionObj Pointer to the collision object.
     * @param axisX Output: unit direction of the segment on the ground (X).
     * @param axisY Output: unit direction of the segment on the ground (Y).
     * @param halfLength Output: half of the projected extent along the segment.
     * @param halfWidth Output: half of the projected extent across it.
     * @return True if the footprint could be measured, false otherwise.
     */
	bool GetShapeFootprint(RE::bhkNiCollisionObject* collisionObj, float& axisX, float& axisY, float& halfLength, float& halfWidth);

	/**
     * @brief Holds display info for an actor (used in UI tables).
     */
	struct ActorDisplayInfo
	{
		RE::TESObjectREFR* actor;  ///< Reference to the actor
		std::string name;          ///< Actor name
		std::string formID;        ///< FormID as string
		std::string type;          ///< Type string (e.g., "Actor", "Actor (Dead/Ragdoll)")
		RE::NiPoint3 pos;          ///< Position
		float sqDist;              ///< Squared distance from reference point
	};

	/**
     * @brief Gets the ragdoll center for a dead actor.
     * @param actor Pointer to the actor.
     * @param outCenter Output: center position of the ragdoll.
     * @return True if center was found, false otherwise.
     */
	inline bool GetRagdollCenter(RE::Actor* actor, RE::NiPoint3& outCenter)
	{
		if (!actor || !actor->IsDead())
			return false;
		if (auto root = actor->Get3D(false)) {
			bool found = false;
			RE::NiPoint3 ragdollCenter;
			RE::BSVisit::TraverseScenegraphCollision(root, [&](RE::bhkNiCollisionObject* a_object) -> RE::BSVisit::BSVisitControl {
				float radius = 0.0f;
				RE::NiPoint3 centerPos;
				if (Util::GetShapeBound(a_object, centerPos, radius)) {
					ragdollCenter = centerPos;
					found = true;
					return RE::BSVisit::BSVisitControl::kStop;
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			if (found) {
				outCenter = ragdollCenter;
				return true;
			}
		}
		return false;
	}

	/**
     * @brief Fills outInfo with display info for an actor, including ragdoll center if dead.
     * @param ref Reference to the actor.
     * @param eyePos Position to use for distance calculation.
     * @param trackRagdolls Whether to include ragdoll info for dead actors.
     * @param outInfo Output: filled display info struct.
     * @return True if info was filled, false otherwise.
     */
	inline bool GetActorDisplayInfo(RE::TESObjectREFR* ref, const RE::NiPoint3& eyePos, bool trackRagdolls, ActorDisplayInfo& outInfo)
	{
		if (!ref)
			return false;
		auto actor = static_cast<RE::Actor*>(ref);
		outInfo.actor = ref;
		outInfo.name = ref->GetName();
		outInfo.formID = std::format("{:X}", ref->GetFormID());
		outInfo.type = "Actor";
		if (actor && actor->IsDead()) {
			if (!trackRagdolls)
				return false;
			outInfo.type = "Actor (Dead/Ragdoll)";
			RE::NiPoint3 pos;
			if (!GetRagdollCenter(actor, pos)) {
				pos = actor->GetPosition();
			}
			outInfo.pos = pos;
		} else {
			outInfo.pos = ref->GetPosition();
		}
		outInfo.sqDist = outInfo.pos.GetSquaredDistance(eyePos);
		return true;
	}
}
