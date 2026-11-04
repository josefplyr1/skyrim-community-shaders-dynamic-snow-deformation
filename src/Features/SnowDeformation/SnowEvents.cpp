// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "Features/SnowDeformation.h"

#include "Globals.h"
#include "State.h"

namespace
{
	constexpr const char* kSnowEventNames[] = { "kick-up", "crumble", "landing", "knockdown", "rush", "body drop", "shout", "blast", "lightning" };
	constexpr float kGravityMetres = 9.8f;
	constexpr float kDropSettleShare = 0.25f;
	constexpr uint32_t kTrackerExpiryFrames = 300;

	uint32_t MixSeed(uint64_t a_key)
	{
		a_key ^= a_key >> 33;
		a_key *= 0xff51afd7ed558ccdull;
		a_key ^= a_key >> 33;
		return uint32_t(a_key);
	}
}

void SnowDeformation::RequestSnowEvents()
{
	snowEventRequestFrame = globals::state->frameCount;
}

bool SnowDeformation::SnowEventsWanted() const
{
	return snowEventRequestFrame != UINT32_MAX && globals::state->frameCount - snowEventRequestFrame <= 2;
}

void SnowDeformation::PushSnowEvent(const SnowEvent& a_event)
{
	if (snowEventsBuilding.size() >= kMaxSnowEventsPerFrame)
		return;
	snowEventsBuilding.push_back(a_event);
}

void SnowDeformation::CollectActorSnowEvents(RE::Actor* a_actor, bool a_dead, const RE::NiPoint3& a_position, float a_groundZ,
	const StampBones* a_bones, const RE::NiAVObject* a_root)
{
	if (!a_actor || !a_root)
		return;
	const uint32_t frame = globals::state->frameCount;
	const uint32_t formID = a_actor->formID;
	const double now = passClockSeconds;
	const float texel = deformWorldSize / float(std::max(deformMapDim, 1u));
	const float stickiness = GetSnowCondition().stickiness;
	auto& body = snowEventBodies[formID];

	if (a_dead) {
		body.trackTime = -1.0;
		body.knocked = false;
		body.rushing = false;
		const auto& bound = a_root->worldBound;
		const float radius = std::max(bound.radius, 1.0f);
		const float dt = float(now - body.time);
		if (!body.valid || dt > 5.0f || std::abs(bound.center.z - body.centreZ) > 8.0f * radius) {
			body.peakFall = 0.0f;
			body.descent = 0.0f;
		} else if (dt > 1e-4f) {
			const float fall = (body.centreZ - bound.center.z) / dt;
			if (fall > 0.0f) {
				body.peakFall = std::max(body.peakFall, fall);
				body.descent += body.centreZ - bound.center.z;
			}
			if (body.peakFall > 0.0f && fall < kDropSettleShare * body.peakFall) {
				float surface = 0.0f;
				if (body.descent >= texel && SampleShellSurface(bound.center.x, bound.center.y, surface) &&
					bound.center.z - radius < surface) {
					SnowEvent event;
					event.kind = SnowEventKind::BodyDrop;
					event.position = { bound.center.x, bound.center.y, std::min(bound.center.z, surface) };
					event.velocity = { 0.0f, 0.0f, -body.peakFall };
					event.strength = std::clamp(body.peakFall / std::sqrt(2.0f * kGravityMetres * kUnitsPerMeter * radius), 0.0f, 1.0f);
					event.amount = body.peakFall;
					event.surfaceZ = surface;
					event.snowDepth = std::max(surface - a_groundZ, 0.0f);
					event.stickiness = stickiness;
					event.formID = formID;
					event.seed = MixSeed((uint64_t(formID) << 32) | frame);
					PushSnowEvent(event);
				}
				body.peakFall = 0.0f;
				body.descent = 0.0f;
			}
		}
		body.centreZ = bound.center.z;
		body.time = now;
		body.frame = frame;
		body.valid = true;
		return;
	}

	float surface = 0.0f;
	const bool inSnow = SampleShellSurface(a_position.x, a_position.y, surface) && a_position.z < surface;
	const float bodyDt = float(now - body.time);
	RE::NiPoint3 velocity{};
	if (body.valid && frame - body.frame <= 2 && bodyDt > 1e-4f)
		velocity = { (a_position.x - body.prev.x) / bodyDt, (a_position.y - body.prev.y) / bodyDt, (a_position.z - body.prev.z) / bodyDt };
	body.prev = a_position;
	body.time = now;
	body.frame = frame;
	body.valid = true;
	body.peakFall = 0.0f;
	body.descent = 0.0f;
	body.centreZ = a_root->worldBound.center.z;

	if (auto it = impactTracks.find(formID); it != impactTracks.end() && it->second.time != body.trackTime) {
		const auto& track = it->second;
		body.trackTime = track.time;
		auto bodyEvent = [&](SnowEventKind a_kind, float a_strength, float a_amount) {
			SnowEvent event;
			event.kind = a_kind;
			event.position = { a_position.x, a_position.y, a_position.z };
			event.velocity = velocity;
			event.strength = std::clamp(a_strength, 0.0f, 1.0f);
			event.amount = a_amount;
			event.surfaceZ = surface;
			event.snowDepth = std::max(surface - a_groundZ, 0.0f);
			event.stickiness = stickiness;
			event.formID = formID;
			event.seed = MixSeed((uint64_t(formID) << 32) | (frame ^ (uint32_t(a_kind) << 28)));
			PushSnowEvent(event);
		};
		if (inSnow) {
			if (track.landingEnergy > 0.0f)
				bodyEvent(SnowEventKind::Landing, track.landingEnergy, track.landingSpeed);
			if (track.knockEnergy > 0.0f && !body.knocked)
				bodyEvent(SnowEventKind::Knockdown, track.knockEnergy, track.speed);
			if (track.speedEnergy >= kRushEnergy && !body.rushing)
				bodyEvent(SnowEventKind::Rush, track.speedEnergy, track.speed);
		}
		body.knocked = track.knockEnergy > 0.0f;
		if (track.speedEnergy >= kRushEnergy)
			body.rushing = true;
		else if (track.speedEnergy < kRushRearm)
			body.rushing = false;
	}

	if (!a_bones)
		return;
	const size_t feet = std::min<size_t>(a_bones->feet.size(), 8);
	for (size_t i = 0; i < feet; i++) {
		auto* node = a_bones->feet[i].node.get();
		if (!node || node->world.scale < 0.01f)
			continue;
		const RE::NiPoint3 footPos = node->world.translate;
		auto& foot = snowEventFeet[(uint64_t(formID) << 8) | i];
		float footSurface = 0.0f;
		const float layer = SampleShellSurface(footPos.x, footPos.y, footSurface) ? footSurface - a_groundZ : 0.0f;
		const float submerged = layer > 1.0f ? std::clamp(footSurface - footPos.z, 0.0f, layer) : 0.0f;
		const float dt = float(now - foot.time);
		if (foot.frame != 0 && frame - foot.frame <= 2 && dt > 1e-4f) {
			const float dx = footPos.x - foot.prev.x;
			const float dy = footPos.y - foot.prev.y;
			const float advance = std::sqrt(dx * dx + dy * dy);
			if (advance > kFootTeleportSpeed * dt) {
				foot.travel = 0.0f;
				foot.pushed = 0.0f;
				foot.wallHeight = 0.0f;
				foot.inSnow = false;
			} else if (submerged > 0.0f) {
				foot.travel += advance;
				foot.pushed += advance * submerged;
				foot.wallHeight = std::max(foot.wallHeight, submerged);
				if (foot.travel >= texel) {
					SnowEvent event;
					event.kind = SnowEventKind::KickUp;
					event.position = footPos;
					event.velocity = { dx / dt, dy / dt, (footPos.z - foot.prev.z) / dt };
					event.strength = std::clamp(foot.pushed / foot.travel / layer, 0.0f, 1.0f);
					event.amount = foot.pushed;
					event.surfaceZ = footSurface;
					event.snowDepth = layer;
					event.stickiness = stickiness;
					event.formID = formID;
					event.seed = MixSeed((uint64_t(formID) << 32) | (frame ^ uint32_t(i << 24)));
					PushSnowEvent(event);
					foot.travel = 0.0f;
					foot.pushed = 0.0f;
				}
			} else if (foot.inSnow && foot.wallHeight > 0.0f && layer > 1.0f) {
				SnowEvent event;
				event.kind = SnowEventKind::Crumble;
				event.position = { foot.prev.x, foot.prev.y, std::min(foot.prev.z, footSurface) };
				event.strength = std::clamp(foot.wallHeight / layer, 0.0f, 1.0f) * (1.0f - stickiness);
				event.amount = foot.wallHeight;
				event.surfaceZ = footSurface;
				event.snowDepth = layer;
				event.stickiness = stickiness;
				event.formID = formID;
				event.seed = MixSeed((uint64_t(formID) << 32) | (frame ^ uint32_t(i << 24) ^ 0x5A5A5Au));
				PushSnowEvent(event);
			}
		}
		if (submerged <= 0.0f) {
			foot.wallHeight = 0.0f;
			foot.travel = 0.0f;
			foot.pushed = 0.0f;
		}
		foot.inSnow = submerged > 0.0f;
		foot.prev = footPos;
		foot.time = now;
		foot.frame = frame;
	}
}

void SnowDeformation::TickSnowEvents()
{
	static_assert(std::size(kSnowEventNames) == size_t(SnowEventKind::Count));
	const uint32_t frame = globals::state->frameCount;
	if (!SnowEventsWanted()) {
		if (!snowEventsPublished.empty() || !snowEventsBuilding.empty() || !snowEventFeet.empty() || !snowEventBodies.empty() || !snowEventMarksSeen.empty()) {
			snowEventsPublished.clear();
			snowEventsBuilding.clear();
			snowEventFeet.clear();
			snowEventBodies.clear();
			snowEventMarksSeen.clear();
		}
		return;
	}

	snowEventsPublished.swap(snowEventsBuilding);
	snowEventsBuilding.clear();

	if (frame % kTrackerExpiryFrames == 0) {
		std::erase_if(snowEventFeet, [frame](const auto& a_entry) { return frame - a_entry.second.frame > kTrackerExpiryFrames; });
		std::erase_if(snowEventBodies, [frame](const auto& a_entry) { return frame - a_entry.second.frame > kTrackerExpiryFrames; });
	}
}

