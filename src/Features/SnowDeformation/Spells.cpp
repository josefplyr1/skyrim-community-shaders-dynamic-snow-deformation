// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "Features/SnowDeformation.h"

#include "Globals.h"
#include "Utils/Game.h"

static constexpr float kSpellReferenceMagnitude = 8.0f;
static constexpr float kSpellMagnitudeMin = 0.35f;
static constexpr float kSpellMagnitudeMax = 3.0f;
static constexpr float kElementalImmunity = 90.0f;
static constexpr float kSelfAreaToUnits = 21.0f;
static constexpr float kSelfAreaReachMin = 60.0f;
static constexpr float kSelfAreaReachMax = 1400.0f;
SnowDeformation::SpellElement SnowDeformation::ClassifyElement(const RE::EffectSetting* a_effect)
{
	if (!a_effect)
		return SpellElement::None;
	switch (a_effect->data.resistVariable) {
	case RE::ActorValue::kResistFire:
		return SpellElement::Fire;
	case RE::ActorValue::kResistFrost:
		return SpellElement::Frost;
	case RE::ActorValue::kResistShock:
		return SpellElement::Shock;
	default:
		break;
	}
	if (a_effect->HasKeywordString("MagicDamageFire"))
		return SpellElement::Fire;
	if (a_effect->HasKeywordString("MagicDamageFrost"))
		return SpellElement::Frost;
	if (a_effect->HasKeywordString("MagicDamageShock"))
		return SpellElement::Shock;
	return SpellElement::None;
}

bool SnowDeformation::AuraFromSpellList(const RE::TESSpellList* a_list, InnateAuraRecord& a_out)
{
	const auto* effects = a_list ? a_list->actorEffects : nullptr;
	if (!effects || !effects->spells)
		return false;

	float immunity[4] = {};
	bool weakness[4] = {};
	auto elementOfAV = [](RE::ActorValue a_av) {
		switch (a_av) {
		case RE::ActorValue::kResistFire:
			return SpellElement::Fire;
		case RE::ActorValue::kResistFrost:
			return SpellElement::Frost;
		case RE::ActorValue::kResistShock:
			return SpellElement::Shock;
		default:
			return SpellElement::None;
		}
	};

	for (uint32_t i = 0; i < effects->numSpells; i++) {
		const RE::SpellItem* spell = effects->spells[i];
		if (!spell)
			continue;
		if (spell->data.spellType != RE::MagicSystem::SpellType::kAbility)
			continue;
		for (const auto* item : spell->effects) {
			const RE::EffectSetting* base = item ? item->baseEffect : nullptr;
			if (!base)
				continue;

			if (base->data.delivery == RE::MagicSystem::Delivery::kSelf &&
				base->data.archetype == RE::EffectSetting::Archetype::kCloak) {
				const SpellElement element = ClassifyElement(base);
				if (element != SpellElement::None) {
					a_out.element = element;
					a_out.rateScale = std::clamp(item->effectItem.magnitude / kSpellReferenceMagnitude,
						kSpellMagnitudeMin, kSpellMagnitudeMax);
					a_out.reachOverride = item->effectItem.area > 0 ?
					                          std::clamp(static_cast<float>(item->effectItem.area) * kSelfAreaToUnits,
												  kSelfAreaReachMin, kSelfAreaReachMax) :
					                          0.0f;
					return true;
				}
			}

			const SpellElement affinity = elementOfAV(base->data.primaryAV);
			if (affinity == SpellElement::None)
				continue;
			const size_t slot = static_cast<size_t>(affinity);
			if (base->IsDetrimental())
				weakness[slot] = true;
			else
				immunity[slot] = std::max(immunity[slot], item->effectItem.magnitude);
		}
	}

	SpellElement made = SpellElement::None;
	float best = kElementalImmunity;
	for (size_t slot = 1; slot < 4; slot++)
		if (immunity[slot] >= best) {
			bool weakElsewhere = false;
			for (size_t other = 1; other < 4; other++)
				weakElsewhere = weakElsewhere || (other != slot && weakness[other]);
			if (weakElsewhere) {
				best = immunity[slot];
				made = static_cast<SpellElement>(slot);
			}
		}
	if (made == SpellElement::None)
		return false;

	a_out.element = made;
	a_out.rateScale = 1.0f;
	a_out.reachOverride = 0.0f;
	return true;
}

const SnowDeformation::InnateAuraRecord* SnowDeformation::ResolveInnateAura(RE::Actor* a_actor)
{
	auto* race = a_actor ? a_actor->GetRace() : nullptr;
	if (!race)
		return nullptr;

	auto raceIt = innateAuraByRace.find(race->formID);
	if (raceIt == innateAuraByRace.end()) {
		InnateAuraRecord record{};
		AuraFromSpellList(race, record);
		if (innateAuraByRace.size() > 512)
			innateAuraByRace.clear();
		raceIt = innateAuraByRace.insert_or_assign(race->formID, record).first;
	}
	if (raceIt->second.element != SpellElement::None)
		return &raceIt->second;

	auto* base = a_actor->GetActorBase();
	if (!base)
		return nullptr;
	auto baseIt = innateAuraByBase.find(base->formID);
	if (baseIt == innateAuraByBase.end()) {
		InnateAuraRecord record{};
		AuraFromSpellList(base, record);
		if (innateAuraByBase.size() > 2048)
			innateAuraByBase.clear();
		baseIt = innateAuraByBase.insert_or_assign(base->formID, record).first;
	}
	return baseIt->second.element != SpellElement::None ? &baseIt->second : nullptr;
}

