#include "Features/SnowDeformation.h"

#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include "Features/SnowDeformation/AlphaBuild.h"
#include "State.h"
#include "Utils/Game.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "feature.snow_deformation."

namespace
{
	// Lays text out from the cursor, wrapping at the content edge; continuation lines hang one indent past the first.
	// Works after SameLine, so it also serves as a widget label.
	void WrapText(const char* a_text, const char* a_end = nullptr)
	{
		ImGuiWindow* window = ImGui::GetCurrentWindow();
		if (window->SkipItems)
			return;
		if (!a_end)
			a_end = a_text + std::strlen(a_text);
		ImGuiContext& g = *GImGui;
		const ImVec2 origin(window->DC.CursorPos.x, window->DC.CursorPos.y + window->DC.CurrLineTextBaseOffset);
		const float right = window->DC.CursorPos.x + ImGui::GetContentRegionAvail().x;
		const float hang = g.FontSize * 1.5f;
		float x = origin.x;
		float y = origin.y;
		float maxX = origin.x;
		const char* s = a_text;
		do {
			const char* lineEnd = static_cast<const char*>(std::memchr(s, '\n', a_end - s));
			if (!lineEnd)
				lineEnd = a_end;
			const char* cut = g.Font->CalcWordWrapPosition(g.FontSize, s, lineEnd, ImMax(right - x, g.FontSize));
			if (cut <= s)
				cut = lineEnd;
			ImGui::RenderText({ x, y }, s, cut, false);
			maxX = ImMax(maxX, x + ImGui::CalcTextSize(s, cut).x);
			y += g.FontSize;
			s = cut;
			while (s < a_end && *s == ' ')
				s++;
			if (s < a_end && *s == '\n')
				s++;
			x = origin.x + hang;
		} while (s < a_end);
		const ImVec2 size(maxX - origin.x, y - origin.y);
		ImGui::ItemSize(size, 0.0f);
		ImGui::ItemAdd(ImRect(origin.x, origin.y, origin.x + size.x, origin.y + size.y), 0);
	}

	std::string FormatV(const char* a_fmt, va_list a_args)
	{
		va_list copy;
		va_copy(copy, a_args);
		const int n = std::vsnprintf(nullptr, 0, a_fmt, copy);
		va_end(copy);
		if (n <= 0)
			return {};
		std::string out(static_cast<size_t>(n), '\0');
		std::vsnprintf(out.data(), out.size() + 1, a_fmt, a_args);
		return out;
	}

	void WrapTextF(const char* a_fmt, ...)
	{
		va_list args;
		va_start(args, a_fmt);
		const std::string text = FormatV(a_fmt, args);
		va_end(args);
		WrapText(text.data(), text.data() + text.size());
	}

	void WrapTextColoredF(const ImVec4& a_color, const char* a_fmt, ...)
	{
		va_list args;
		va_start(args, a_fmt);
		const std::string text = FormatV(a_fmt, args);
		va_end(args);
		ImGui::PushStyleColor(ImGuiCol_Text, a_color);
		WrapText(text.data(), text.data() + text.size());
		ImGui::PopStyleColor();
	}

	void WrapTextDisabledF(const char* a_fmt, ...)
	{
		va_list args;
		va_start(args, a_fmt);
		const std::string text = FormatV(a_fmt, args);
		va_end(args);
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		WrapText(text.data(), text.data() + text.size());
		ImGui::PopStyleColor();
	}

#if !SNOW_ALPHA_BUILD
	// Widget drawn with a hidden label, its label wrapped beside it. The widget stays the last item, and hovering
	// the label counts as hovering it, so tooltips and IsItem* queries behave as with a native label.
	template <class F>
	bool WrappedLabel(const char* a_label, bool* a_toggle, F&& a_widget)
	{
		ImGuiContext& g = *GImGui;
		ImGui::PushID(a_label);
		bool changed = a_widget("##w");
		const ImGuiLastItemData widget = g.LastItemData;
		ImGui::SameLine(0.0f, g.Style.ItemInnerSpacing.x);
		WrapText(a_label, ImGui::FindRenderedTextEnd(a_label));
		const ImRect labelRect = g.LastItemData.Rect;
		bool labelHovered = (g.LastItemData.StatusFlags & ImGuiItemStatusFlags_HoveredRect) != 0;
		if (a_toggle) {
			const ImGuiID id = ImGui::GetID("##l");
			if (ImGui::ItemAdd(labelRect, id)) {
				bool hovered = false;
				bool held = false;
				if (ImGui::ButtonBehavior(labelRect, id, &hovered, &held)) {
					*a_toggle = !*a_toggle;
					changed = true;
				}
				labelHovered = hovered;
			}
		}
		ImGui::PopID();
		g.LastItemData = widget;
		if (labelHovered)
			g.LastItemData.StatusFlags |= ImGuiItemStatusFlags_HoveredRect;
		return changed;
	}

	bool CheckboxWrapped(const char* a_label, bool* a_v)
	{
		return WrappedLabel(a_label, a_v, [&](const char* a_id) { return ImGui::Checkbox(a_id, a_v); });
	}

	bool SliderFloatWrapped(const char* a_label, float* a_v, float a_min, float a_max, const char* a_format)
	{
		return WrappedLabel(a_label, nullptr, [&](const char* a_id) { return ImGui::SliderFloat(a_id, a_v, a_min, a_max, a_format); });
	}

	bool SliderIntWrapped(const char* a_label, int* a_v, int a_min, int a_max)
	{
		return WrappedLabel(a_label, nullptr, [&](const char* a_id) { return ImGui::SliderInt(a_id, a_v, a_min, a_max); });
	}
#endif
}

namespace
{
	// Low = every lever at its cheapest, High = the shipped defaults, Medium halfway.
	struct QualityPreset
	{
		uint32_t mapDim;
		uint32_t objectTrenchDim;
		float trenchesM;
		float actorTrenchesM;
		float skinsM;
		float parallaxDepth;
		float parallaxShadow;
		float skinTessCapPx;
		bool sparkle;
		bool contactShadows;
		bool selfShadowing;
		bool nearDetail;
		bool castsShadows;
	};
	constexpr QualityPreset kQualityPresets[SnowDeformation::kQualityPresetCount] = {
		{ 1024u, 512u, 29.0f, 10.0f, SnowDeformation::kMinRangeSkinsM, 0.0f, 0.0f, 32.0f, false, false, false, false, false },
		{ 2048u, 1024u, 115.0f, 105.0f, 150.0f, 1.0f, 0.5f, 24.0f, false, false, true, false, true },
		{ 4096u, 2048u, 200.0f, 200.0f, SnowDeformation::kMaxRangeSkinsM, 1.0f, 0.5f, 16.0f, true, true, true, true, true },
	};
}

void SnowDeformation::ApplyQualityPreset(int a_preset)
{
	const auto& p = kQualityPresets[std::clamp(a_preset, 0, kQualityPresetCount - 1)];
	if (settings.DeformMapResolution != p.mapDim) {
		settings.DeformMapResolution = p.mapDim;
		deformMapDimDirty = true;
	}
	if (settings.RangeTrenchesM != p.trenchesM) {
		settings.RangeTrenchesM = p.trenchesM;
		trenchRangeDirty = true;
	}
	settings.ObjectTrenchResolution = p.objectTrenchDim;
	settings.RangeActorTrenchesM = p.actorTrenchesM;
	settings.RangeSkinsM = p.skinsM;
	settings.Tessellation = true;
	settings.ParallaxDepth = p.parallaxDepth;
	settings.ParallaxShadowStrength = p.parallaxShadow;
	settings.SkinTessCapPx = p.skinTessCapPx;
	settings.VolumeSnow = false;
	settings.SnowGlints = p.sparkle;
	settings.SnowScreenSpaceShadows = p.contactShadows;
	settings.ShellHorizonMarch = p.selfShadowing;
	settings.ObjectSnowNearDetail = p.nearDetail;
	settings.SnowCastsShadows = p.castsShadows;
}

int SnowDeformation::MatchingQualityPreset() const
{
	for (int i = 0; i < kQualityPresetCount; i++) {
		const auto& p = kQualityPresets[i];
		if (settings.DeformMapResolution == p.mapDim && settings.ObjectTrenchResolution == p.objectTrenchDim && settings.RangeTrenchesM == p.trenchesM &&
			settings.RangeActorTrenchesM == p.actorTrenchesM && settings.RangeSkinsM == p.skinsM &&
			settings.Tessellation && settings.ParallaxDepth == p.parallaxDepth && settings.ParallaxShadowStrength == p.parallaxShadow &&
			settings.SkinTessCapPx == p.skinTessCapPx && !settings.VolumeSnow &&
			settings.SnowGlints == p.sparkle && settings.SnowScreenSpaceShadows == p.contactShadows && settings.ShellHorizonMarch == p.selfShadowing &&
			settings.ObjectSnowNearDetail == p.nearDetail && settings.SnowCastsShadows == p.castsShadows)
			return i;
	}
	return -1;
}

void SnowDeformation::DrawSettings()
{
	ImGui::TextDisabled("%s", T(TKEY("credit"), "Snow Deformation - by josefplyr1"));
	if (auto _ttCredit = Util::HoverTooltipWrapper())
		ImGui::Text("%s", kAttribution);
	ImGui::Separator();

	ImGui::Checkbox(T(TKEY("enable"), "Enable Snow Deformation"), &settings.EnableSnowDeformation);

	{
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(T(TKEY("quality_presets"), "Quality Preset:"));
		if (auto _ttPresets = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("quality_presets_tooltip"), "Sets the performance settings in one click. High is the default: every effect on, every reach at its maximum and a 4096 Deformation Map. Medium keeps the snow's shadows (Snow Casts Shadows, Snow Self-Shadowing) and turns off the close-up extras (Snow Sparkle, Snow Contact Shadows, Object Snow Near Detail), with Trenches at 115 m, NPC Trenches at 105 m, Object Snow at 150 m, a 24 px Object Snow Tessellation Cap and Snow Trenches on Objects at 22 m. Low turns all five off, and parallax too, with a 1024 Deformation Map, Trenches at 29 m, Object Snow at 50 m, NPC Trenches at 10 m, a 32 px cap and Snow Trenches on Objects at 11 m. Every preset turns Tessellate Trenches on and the experimental Volume Snow off. Everything else keeps its value; change any of these afterwards and the preset shows as Custom. Applying can clear existing trenches (a resolution or Trenches-range change does); remembered trenches are re-injected."));
		const char* presetLabels[kQualityPresetCount] = { T(TKEY("preset_low"), "Low"), T(TKEY("preset_medium"), "Medium"), T(TKEY("preset_high"), "High") };
		const int activePreset = MatchingQualityPreset();
		for (int i = 0; i < kQualityPresetCount; i++) {
			ImGui::SameLine();
			if (i == activePreset)
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
			if (ImGui::Button(presetLabels[i]))
				ApplyQualityPreset(i);
			if (i == activePreset)
				ImGui::PopStyleColor();
		}
		if (activePreset < 0) {
			ImGui::SameLine();
			ImGui::TextDisabled("%s", T(TKEY("preset_custom"), "(Custom)"));
		}
	}

	if (ImGui::TreeNodeEx(T(TKEY("general_settings"), "General Settings"), ImGuiTreeNodeFlags_Framed)) {
		ImGui::InputText(T(TKEY("snow_texture_path"), "Shell Snow Texture"), &settings.SnowTexturePath);
		if (auto _ttTex = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("snow_texture_path_tooltip"), "DDS path (relative to Data) for the shell's snow diffuse. Point it at the modlist's snow texture, then press Reload."));
		ImGui::SameLine();
		if (ImGui::Button(T(TKEY("reload_texture"), "Reload"))) {
			shellSnowDiffuseSRV = nullptr;
			shellSnowNormalSRV = nullptr;
			shellSnowRmaosSRV = nullptr;
			shellSnowTextureIsPBR = false;
			shellSnowTextureAttempted = false;
		}

		if (ImGui::InputText(T(TKEY("snow_pbr_config"), "Snow PBR Config"), &settings.SnowPBRConfig)) {
			// Re-resolve on the next frame, the way Reload does: the match is
			// made where the texture loads.
			shellSnowTextureAttempted = false;
		}
		if (auto _ttPbrCfg = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("snow_pbr_config_tooltip"), "The shell takes its roughness, specular level and sparkle from a TruePBR texture set, matched by the snow texture's file name. A modlist often holds several sets whose name contains that word - a blend variant, a glacier, a grass edge - and some of them carry no sparkle settings at all, which leaves the snow flat and dull. Leave this empty to take the best automatic match, or type a set's name (its JSON file name, without the extension) to pin it. The set in use is shown beside this box and updates as you type."));
		ImGui::SameLine();
		if (snowPBRSetName.empty())
			ImGui::TextDisabled("%s", T(TKEY("snow_pbr_config_none"), "(no match - built-in snow)"));
		else
			ImGui::TextDisabled("%s%s", T(TKEY("snow_pbr_config_using"), "using: "), snowPBRSetName.c_str());

		ImGui::Checkbox(T(TKEY("snow_glints"), "Snow Sparkle"), &settings.SnowGlints);
		if (auto _ttGlints = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("snow_glints_tooltip"), "The glitter of light catching single snow crystals, on the ground snow and on object snow, using Community Shaders' TruePBR glints (needs TruePBR). The Medium and Low presets turn it off."));
		ImGui::Checkbox(T(TKEY("snow_screen_space_shadows"), "Snow Contact Shadows"), &settings.SnowScreenSpaceShadows);
		if (auto _ttSnowSSS = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("snow_screen_space_shadows_tooltip"), "Community Shaders' Screen-Space Shadows on the ground snow and object snow: the small, sharp shadows where grass, feet and objects meet the snow (needs Screen-Space Shadows). Off, the snow is shaded by the sun's shadows and its own self-shadowing only. The Medium and Low presets turn it off."));
		ImGui::Checkbox(T(TKEY("snow_casts_shadows"), "Snow Casts Shadows"), &settings.SnowCastsShadows);
		if (auto _ttSnowCasts = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("snow_casts_shadows_tooltip"), "The ground snow and road snow are drawn into the sun's shadows, so deep snow, banks and trench walls cast shadows onto the world and onto the snow itself. Off, the snow still receives shadows but casts none. The Low preset turns it off."));

		ImGui::PushID("general_settings");
		if (ImGui::TreeNodeEx(T(TKEY("menu_advanced"), "Advanced"))) {
			ImGui::Checkbox(T(TKEY("camera_above_snow"), "Keep Camera Above the Snow"), &settings.CameraAboveSnow);
			if (auto _ttCam = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("camera_above_snow_tooltip"), "The game's camera collides with the ground, not with the snow, so a low third-person camera, or the camera while you are knocked down, can slip under the sheet and show its underside. On, the camera is pulled in toward you when that happens, the way it is against a hillside, and lifted clear of the snow while you are down."));

			ImGui::Checkbox(T(TKEY("shell_horizon_march"), "Snow Self-Shadowing"), &settings.ShellHorizonMarch);
			if (auto _ttHm = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("shell_horizon_march_tooltip"), "The snow shades itself: bumps, drifts, berms and trench rims cast soft shadows onto the snow behind them, found by a short march toward the sun over the snow's height, on the ground snow and on object snow. It fades out as the sun nears the horizon, where the game's own shadows take over. Off, the snow is shaded by the game's shadows and Snow Contact Shadows only. The Low preset turns it off."));
			ImGui::SliderFloat(T(TKEY("shelter_max_height"), "Shelter Max Height"), &settings.ShelterMaxHeight, 100.0f, 1000.0f, "%.0f units");
			if (auto _ttShelterMax = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("shelter_max_height_tooltip"), "Snow under a roof, bridge or archway thins out, because the structure keeps the snowfall off. A structure whose underside is higher above the ground than this no longer counts: a wide archway with plenty of air under it is as open as the sky, and the snow beneath it keeps its full depth. Fades out over the next 100 units. 70 units is about a metre."));
			ImGui::SliderFloat(T(TKEY("shelter_drift_angle"), "Snow Drift Angle"), &settings.ShelterDriftAngle, 5.0f, 70.0f, "%.0f deg");
			if (auto _ttShelterAngle = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("shelter_drift_angle_tooltip"), "Cover higher than Shelter Max Height (up to about 250 units) still keeps the snow off where the spot is enclosed - cover all around it, like the inside of a tent; a spot open along one side, like the strip under an overhang, keeps its snow. This angle is how far from straight down the snow blows in, which sets how much cover a spot needs around it to count as enclosed. Higher = snow reaches further in, so only deep cover shelters; lower = a smaller roof is enough. Default 30."));
			ImGui::SliderFloat(T(TKEY("workspace_clearing_size"), "Workspace Clearing Size"), &settings.TrampleZoneScale, 0.25f, 2.0f, "%.2fx");
			if (auto _ttWcs = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("workspace_clearing_size_tooltip"), "Radius multiplier for the snow bowls around workstations, smelters, forges, stalls, wells and shrines. Applies within a second."));

			ImGui::SliderFloat(T(TKEY("workspace_clearing_height"), "Workspace Clearing Height"), &settings.TrampleZoneHeight, 0.0f, 100.0f, "%.0f%%");
			if (auto _ttWch = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("workspace_clearing_height_tooltip"), "Snow height remaining in a workspace bowl, as a percent of the surrounding depth. 0 melts to the floor, 100 disables the clearing. Applies within a second."));

			ImGui::SliderFloat(T(TKEY("object_meet_band"), "Object Meeting Blend"), &settings.ObjectMeetBand, 0.0f, 64.0f, "%.0f units");
			if (auto _ttMeet = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("object_meet_band_tooltip"), "Where the ground snow meets a snow-covered object, the two blend toward each other so they read as one surface instead of meeting at a lit line. This is the width of that meeting, split in half: at the default 12, the ground snow's shading eases toward the object's slope over its last 6 units, and the object's snow eases toward the ground's over the first 6 units up from it, meeting in the middle. Higher = a wider, softer meeting; 0 = off, the two meet as they are. Shading only - no geometry moves. 70 units is about a metre."));

			ImGui::TreePop();
		}
		ImGui::PopID();

		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("model_depths"), "Object Snow"), ImGuiTreeNodeFlags_Framed)) {
		if (auto _ttModels = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("model_depths_tooltip"), "Object snow: the recolor of the game's projected snow and the dials that shape the drape over it. Flat vs round is classified automatically per mesh."));

		// Object snow is the recolor of the game's own projected snow and the
		// drape the skin lays over it; nothing of ours stands above a mesh.
		ImGui::Checkbox(T(TKEY("object_snow_near_detail"), "Object Snow Near Detail"), &settings.ObjectSnowNearDetail);
		if (auto _ttNearDetail = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("object_snow_near_detail_tooltip"), "Near you - over a quarter of the Object Height Map Range, 25 m at its default - road snow and the trenches in it read their shape from a map four times finer: about 1.7 world units a texel instead of 6.8 at the default range. Off, roads use the coarser map everywhere: slightly softer road edges and trenches up close, a little less GPU time. The Medium and Low presets turn it off."));

		ImGui::Checkbox(T(TKEY("proj_snow_match"), "Recolor Projected Snow"), &settings.ProjSnowMatch);
		if (auto _ttPsm = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("proj_snow_match_tooltip"), "Makes the game's painted-on projected snow look like this mod's snow, and owns the DRAPE that does it. Two halves: inside the object's own shader the projection's texture and material are swapped for the snow shell's set, which works from every angle, overhangs included; and near the camera the shell lays its own material over the parts the game paints solidly, which is what gives the drape the shell's snow rather than a tint of it. Edge Lump Reach shapes that second half - how far it spreads past the paint. Every pixel the game paints solidly (its own half blend and up) takes the full snow, all or nothing; a faint dusting stays bare. The drape lies flat on the mesh; nothing of ours stands above an object. Only draws whose projected material really is snow are touched, so sand and moss projections keep their look."));

		ImGui::Checkbox(T(TKEY("snow_textured_recolor"), "Recolor Snow-Textured Meshes"), &settings.SnowTexturedRecolor);
		if (auto _ttStr = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("snow_textured_recolor_tooltip"), "Some meshes wear snow as a plain texture rather than the game's projected paint: dirt cliffs with a snow01 top, rock piles and their skirts, a season mod's winter variants, the vanilla drifts. The projected-snow recolor never sees those, so they keep their own snow texture and mismatch the shell. This gives them the shell's snow in the object's own shader, on the texels that read as snow; a plain ground snow texture (snow01, snowrocks01) takes it on every texel where it lies flat, so the stones in a rock pile's ground turn to snow too. Solid shapes also take the drape's coat over that snow; blended overlays such as rock skirts keep their own soft edge and take no coat. Needs Recolor Projected Snow."));

		ImGui::Checkbox(T(TKEY("proj_snow_normals"), "Recolor Projected Snow Normals"), &settings.ProjSnowNormals);
		if (auto _ttProjNormals = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("proj_snow_normals_tooltip"), "Recolored projected snow also takes the snow's own surface relief, perturbed exactly the way the drape's coat does, so the drape and the bare snow beside it share one grain. Without it the game's projected snow keeps the plank's or the stone's own bump - the game never sends its own snow normal to the deferred buffer - and the snow reads as paint on the surface wherever the drape does not cover it."));

		ImGui::Checkbox(T(TKEY("snow_follows_parallax"), "Snow Follows Parallax"), &settings.SnowFollowsParallax);
		if (auto _ttFollowParallax = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("snow_follows_parallax_tooltip"), "On objects drawn with Extended Materials parallax (PBR displacement and complex parallax), the snow lies on the surface the parallax draws instead of on the flat mesh under it: its edge and its own texture move with the stone as the camera moves, so the stone no longer slides beneath the snow and the snow stops looking like it floats. Both the game's recolored projected snow and the drape's coat over it follow. With Flatten Parallax Under Snow also on, they follow the relief the flatten leaves. The first time it is switched on, the coat compiles its shaders, a stall of a few seconds once. Objects on the older vanilla parallax shader are not affected. On by default, together with the other one."));

		ImGui::Checkbox(T(TKEY("flatten_parallax_under_snow"), "Flatten Parallax Under Snow"), &settings.FlattenParallaxUnderSnow);
		if (auto _ttFlattenParallax = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("flatten_parallax_under_snow_tooltip"), "The other answer to snow floating over parallax objects: on an object with projected snow, its Extended Materials parallax (PBR displacement and complex parallax) fades out as the snow covers the surface, the way real snow fills cracks and crevices, and the shadows the relief casts on itself fade with it. Where the snow lies solid the rock is flat, so the snow, its coat and the tracks in it sit on a surface that does not move; where it lies in patches part of the relief stays; bare rock keeps all of it. With Snow Follows Parallax also on, the patches follow the relief that stays. Objects on the older vanilla parallax shader carry no projected snow and are not affected. On by default, together with the other one."));

		ImGui::Checkbox(T(TKEY("unprojected_shapes_bare"), "Unpainted Shapes Stay Bare"), &settings.UnprojectedShapesBare);
		if (auto _ttBare = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("unprojected_shapes_bare_tooltip"), "A building whose snow is painted on by the game is drawn as several shapes, and some of them carry no snow at all. Those plain shapes are drawn over the snowy ones, and they wipe out the note the game leaves for the drape saying whether a spot was painted. With no note, the drape assumes nothing was ever decided and lays snow anyway - which is how door frames and posts under a roof end up sleeved in snow. On, a plain shape leaves the note \"considered, no snow here\" and the drape stays off it. Only objects with a real snow material are affected; snow-textured mesh replacements and drifts are not."));

		ImGui::Checkbox(T(TKEY("multipass_snow_follows_paint"), "Multipass Snow Follows Paint"), &settings.MultipassSnowFollowsPaint);
		if (auto _ttMultipass = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("multipass_snow_follows_paint_tooltip"), "Objects whose snow material is drawn in two passes (vanilla glaciers and ice sheets, and every snow object under Simplicity of Snow) keep the drape only where the game's own snow pass paints. Off, the drape guesses on them and covers every face, stone included."));

		ImGui::SliderFloat(T(TKEY("seasons_snow_max_angle"), "Seasons Snow Max Angle"), &settings.SeasonsSnowMaxAngle, 0.0f, 90.0f, "%.0f deg");
		if (auto _ttSeasons = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("seasons_snow_max_angle_tooltip"), "Steepest slope that keeps snow on objects Seasons of Skyrim paints at runtime. Those projections come with the record default of 90 degrees and no painted mask; 90 keeps Seasons' own coverage, which the recolor and the drape then follow exactly, and lower cuts the snow off steeper faces for them only. Authored snow objects keep their own angle and mask."));

		{
			// Driven by the accumulation scalar while Recolored Snow Accumulates is on.
			const bool reachDriven = settings.EnableSnowAccumulation && settings.RecoloredSnowAccumulates;
			float reachShown = reachDriven ? snowAccumulation.load(std::memory_order_relaxed) : settings.SkinEdgeFlankWidth;
			ImGui::BeginDisabled(reachDriven);
			if (ImGui::SliderFloat(T(TKEY("edge_lump_reach"), "Edge Lump Reach"), &reachShown, 0.0f, 1.0f, reachDriven ? "%.3f (snow accumulation)" : "%.3f") && !reachDriven)
				settings.SkinEdgeFlankWidth = reachShown;
			ImGui::EndDisabled();
		}
		if (auto _ttEdgeR = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("edge_lump_reach_tooltip"), "How far the snow on rocks and buildings reaches past the patches the game paints, onto the bare surface. At 0 it is exactly the game's paint. Raising it lets the snow creep outward along the game's own falloff - slope, painted alpha and the projection's noise - so thin dustings thicken first and steep faces stay bare longest. The same from every angle. Needs Recolor Projected Snow, and only objects that carry the game's own projected-snow data take part."));
		ImGui::SliderFloat(T(TKEY("steep_face_thinning"), "Steep Face Thinning"), &settings.SteepFaceThinning, 0.0f, 1.0f, "%.2f");
		if (auto _ttSteep = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("steep_face_thinning_tooltip"), "How much less the accumulating snow spreads onto steep faces. Snow does not settle on a vertical post or wall the way it does on a roof: at 1 a vertical face takes none of the spread that Edge Lump Reach (or snowfall) adds, at 0 it takes all of it, and slopes gentler than about 60 degrees always take all of it. The game's own painted snow is never touched - with Edge Lump Reach at 0 this setting does nothing. Needs Recolor Projected Snow."));

		// The object-snow experiments live HERE, beside the sliders they
		// modify, so the whole workbench is one tree (Josef's round-9 ask -
		// no scrolling between Snow Trenches and the model depths).
		ImGui::SliderFloat(T(TKEY("skin_depth_bias"), "Object Snow Depth Bias"), &settings.SkinDepthBias, 0.0f, 128.0f, "%.0f");
		if (auto _ttSdb = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("skin_depth_bias_tooltip"), "How far the object snow layer is nudged toward the camera in the depth buffer, in the buffer's own smallest steps. The layer is rasterised from the same vertices as the object under it, so at a distance the two land on the same depth value and fight - snow that flickers, or shows from one angle and not another. A few steps settle the fight; more than needed makes the layer stand in front of things that should hide it. Constant with distance in depth terms, which is what the contest needs."));

		ImGui::SliderFloat(T(TKEY("skin_slope_depth_bias"), "Object Snow Slope Bias"), &settings.SkinSlopeDepthBias, 0.0f, 4.0f, "%.2f");
		if (auto _ttSsb = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("skin_slope_depth_bias_tooltip"), "The part of the depth bias that grows with how steeply a face is seen. Faces at a grazing angle cover a large depth range per pixel and fight hardest; this scales the nudge with that slope. Capped internally so no angle can push the layer far in front of its object."));

		ImGui::SliderFloat(T(TKEY("skin_tess_cap_px"), "Object Snow Tessellation Cap"), &settings.SkinTessCapPx, 0.0f, 32.0f, "%.0f px");
		if (auto _ttStc = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("skin_tess_cap_px_tooltip"), "Caps how finely road snow is subdivided (no other object snow is): no subdivision segment may be smaller on screen than this many pixels. At a distance, finer segments end up smaller than a pixel and cost GPU time for nothing visible. Higher = cheaper: up to about 20 it only trims the rounded edges at range; above that it coarsens the whole road surface. 0 turns the cap off. The Medium and Low presets raise it."));

		ImGui::SeparatorText(T(TKEY("menu_experimental"), "Experimental"));

		ImGui::Checkbox(T(TKEY("volume_snow"), "Volume Snow"), &settings.VolumeSnow);
		if (auto _ttVol = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("volume_snow_tooltip"), "Rasterises the captured objects into nested windows of voxels around the camera (Ring Shape, Levels), grows a snow field on every surface that faces up, and draws that field as snow - marched per pixel and shaded with the object snow's own material, so it matches the snow on the objects beneath it, which keeps drawing under it. Snow whose shape is not inherited from the object's mesh: it can overhang an edge, bridge two rocks, cap a post. Remembered between frames and fading where nothing redraws. Cost is the Volume - Bake and Volume - Draw rows."));
		if (settings.VolumeSnow) {
			// Says so when the pass is not running: a slice that never
			// updates is indistinguishable from a broken volume otherwise.
			if (voxelShadersFailed)
				WrapTextColoredF({ 1.0f, 0.35f, 0.35f, 1.0f }, "%s", T(TKEY("voxel_status_failed"), "NOT RUNNING: a voxel shader failed to compile, or this GPU cannot run the volume - see CommunityShaders.log."));
			else if (!voxelLevels[0].valid)
				WrapTextDisabledF("%s", T(TKEY("voxel_status_waiting"), "Waiting for the first frame."));
#if !SNOW_ALPHA_BUILD
			ImGui::SliderFloat(T(TKEY("voxel_memory"), "Volume Memory"), &voxelMemorySeconds, 0.5f, 30.0f, "%.1f s");
			if (auto _ttVoxMem = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("voxel_memory_tooltip"), "How long a voxel stays after its object last drew, at 60 fps. Objects behind the camera are not in the capture list, so the volume keeps what it has seen and lets it fade. An object you have not looked at since switching this on is not in the volume yet."));
#endif
			ImGui::SliderFloat(T(TKEY("volume_snow_depth"), "Depth"), &settings.VolumeSnowDepth, 8.0f, 25.0f, "%.0f u");
			if (auto _ttVoxDepth = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("volume_snow_depth_tooltip"), "How high the snow surface sits over an object's top - the same on every ring, near or far, whatever that ring's voxel size. Toward an edge it rounds down over Edge Rounding."));
			ImGui::SliderFloat(T(TKEY("volume_snow_coverage"), "Coverage"), &settings.VolumeSnowCoverage, 0.02f, 0.5f, "%.2f");
			if (auto _ttVoxCov = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("volume_snow_coverage_tooltip"), "How much thinning a lip survives before it ends. Lower = longer overhangs and thin things (rails, posts) keep their caps; higher = the snow ends sooner past an edge. It does not change the depth."));
			ImGui::SliderFloat(T(TKEY("volume_voxel_size"), "Voxel Size"), &settings.VolumeVoxelSize, 1.0f, 24.0f, "%.0f u");
			if (auto _ttVoxSize = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("volume_voxel_size_tooltip"), "How fine the snow field is nearest the camera. Each further level doubles the voxel and doubles the reach, so detail near you and reach far from you are set separately: this slider for detail, Volume Levels for reach. Changing this restarts the volume."));
			{
				const char* shapes[] = { "Cubic (256 x 256 x 256)", "Wide (512 x 512 x 256)", "Wide, flat (512 x 512 x 128)" };
				ImGui::Combo(T(TKEY("volume_ring_shape"), "Ring Shape"), &settings.VolumeRingShape, shapes, 3);
				if (auto _ttVoxShape = Util::HoverTooltipWrapper())
					ImGui::Text("%s", T(TKEY("volume_ring_shape_tooltip"), "The window every ring is cut from. Wide doubles the reach sideways and ahead for four times the memory; wide and flat halves the height for about half of Wide's memory, so eaves above you go to the next ring out. Every ring shares the shape, each still twice the one inside it in every axis."));
			}
			ImGui::SliderInt(T(TKEY("volume_levels"), "Levels"), &settings.VolumeLevels, 1, (int)kVoxelMaxLevels);
			if (auto _ttVoxLevels = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("volume_levels_tooltip"), "Nested grids around the camera - the clipmap. Each level is a Ring Shape window at twice the voxel of the one inside it, and draws only the ring the finer level does not reach, so the reach doubles per level while the near detail stays the base voxel. A full level takes about 226 MB at the default Wide shape (56 MB Cubic, 129 MB Wide, flat), and each level runs its own capture and field passes."));
			ImGui::SliderInt(T(TKEY("volume_fine_levels"), "Fine Levels"), &settings.VolumeFineLevels, 1, (int)kVoxelMaxLevels);
			if (auto _ttVoxFine = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("volume_fine_levels_tooltip"), "How many levels, counting out from the camera, keep the Ring Shape's full grid. The rest use half as many voxels a side at twice the voxel: the same reach for an eighth of the voxels and a fraction of the memory, and one step less detail out where it is too far to see. Changing this rebuilds those levels."));
			{
				const float voxel = std::clamp(settings.VolumeVoxelSize, 1.0f, 24.0f);
				const int levels = std::clamp(settings.VolumeLevels, 1, (int)kVoxelMaxLevels);
				const auto dims = VoxelDimsMax();
				const float nearAcross = dims.x * voxel / kUnitsPerMeter;
				const float nearTall = dims.z * voxel / kUnitsPerMeter;
				const float farAcross = nearAcross * float(1 << (levels - 1));
				// EnsureVoxelResources' allocations. Per ring: the two occupancy volumes and the field (R8), the R32 top
				// heightmap at four texels a voxel side and its under-layers at two, three uints a brick (list, dirty,
				// flags), the dispatch lists and the draw args. Once: the R8 support volume at the largest dims and the slice image.
				double bytes = double(dims.x) * dims.y * dims.z + double(kVoxelDimWide) * kVoxelDimWide;
				for (uint i = 0; i < (uint)levels; i++) {
					const auto d = VoxelDimsForLevel(i);
					const double voxels = double(d.x) * d.y * d.z;
					bytes += 3.0 * voxels + 4.0 * (16.0 + 4.0 * kVoxelUnderLayers) * d.x * d.x + 12.0 * voxels / 512.0 + kVoxelListBytes + 16.0;
				}
				WrapTextF("Finest level %.0f m across, %.0f m tall; outermost %.0f m across (%.0f m around you); %.0f MB", nearAcross, nearTall, farAcross, farAcross * 0.5f, bytes / 1048576.0);
			}
			ImGui::SliderFloat(T(TKEY("volume_march_step"), "March Step"), &settings.VolumeMarchStep, 0.25f, 2.0f, "%.2f voxels");
			if (auto _ttVoxStep = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("volume_march_step_tooltip"), "How far apart the draw samples the snow field along each pixel's ray, in voxels. The hit is refined afterwards, so this only has to find the crossing - but a lip thinner than a step can be stepped over. Halving it doubles the draw's cost."));
			ImGui::SliderFloat(T(TKEY("volume_detail_distance"), "Detail Distance"), &settings.VolumeDetailDistance, 100.0f, 8000.0f, "%.0f u");
			if (auto _ttVoxDetail = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("volume_detail_distance_tooltip"), "Inside this distance the volume snow shades with the object snow's full material. Over the next half of it the parts you cannot see at range fade out: the parallax relief marches, the berm ridges, and the five-tap horizon shadow march. Object snow itself is not affected. Raise it if far volume snow looks flatter than the object snow beside it; lower it for a cheaper draw."));
			ImGui::Checkbox(T(TKEY("volume_skip_empty"), "Skip Empty Cells"), &settings.VolumeSkipEmptyCells);
			if (auto _ttVoxSkip = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("volume_skip_empty_tooltip"), "The ray jumps over the eighths of a brick that hold no snow surface instead of sampling through them. No visible change; off for comparing cost."));
			ImGui::SliderFloat(T(TKEY("volume_forward_bias"), "Forward Bias"), &settings.VolumeForwardBias, 0.0f, 0.45f, "%.2f");
			if (auto _ttVoxFwd = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("volume_forward_bias_tooltip"), "How far ahead of you each ring's window sits, as a fraction of its width. 0 centres it on you; higher puts more of it in front, where you look, and less behind. At 0.35 each ring reaches about 85% of its width ahead of you and 15% behind. Turning around refreshes the near ring the same frame; the far rings take a few."));
			ImGui::SliderFloat(T(TKEY("volume_vertical_bias"), "Vertical Bias"), &settings.VolumeVerticalBias, -0.3f, 0.3f, "%.2f");
			if (auto _ttVoxVert = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("volume_vertical_bias_tooltip"), "How far below the eye each ring's window sits, as a fraction of its height. The third-person camera climbs when you look down, and a window centred on it then loses the ground beside you to the next ring out - the ring colours changing with pitch. Positive puts more of the nearest ring below you, where the fences and steps are, and less above, where the eaves are."));
			ImGui::SliderFloat(T(TKEY("volume_max_distance"), "Max Distance"), &settings.VolumeMaxDistance, 500.0f, 8000.0f, "%.0f u");
			if (auto _ttVoxMaxDist = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("volume_max_distance_tooltip"), "Past this the volume snow dithers out and the object snow carries the object alone. The outermost rings sample objects at their coarsest voxel, and there is a range at which the object snow's own smooth cover simply looks better than that. 8000 = never."));
			ImGui::Checkbox(T(TKEY("volume_staggered_capture"), "Staggered Capture"), &settings.VolumeStaggeredCapture);
			if (auto _ttVoxStagger = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("volume_staggered_capture_tooltip"), "An object already in a ring's volume is re-drawn into it only every 8th rebuild on the nearest ring (4th and 2nd on the next two); its memory keeps it there between. Everything you have not seen yet still draws within that many rebuilds - a few frames near you. Off draws every object into every ring every rebuild, for comparing the Volume - Bake row."));
			ImGui::Checkbox(T(TKEY("volume_dirty_bricks"), "Dirty Bricks"), &settings.VolumeDirtyBricks);
			if (auto _ttVoxDirty = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("volume_dirty_bricks_tooltip"), "A rebuild recomputes the snow only in the columns of bricks whose captured shape changed since the last one - something newly seen, faded, or scrolled in - plus the blur's reach around them; everywhere else keeps last time's snow, which is world-anchored and does not move. Standing still, almost nothing is rebuilt. Every 120th rebuild is whole. Off rebuilds everything every time, for comparing."));
			ImGui::SliderFloat(T(TKEY("volume_snow_overhang"), "Overhang"), &settings.VolumeSnowOverhang, 0.0f, 16.0f, "%.0f u");
			if (auto _ttVoxOverhang = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("volume_snow_overhang_tooltip"), "How far past an edge the snow may jut sideways, in units, whatever its depth - past this it only grows up. Also how readily the snow of two nearby surfaces melds into one. Solid always stops it - snow never spreads through a wall or up a step riser - so this is the reach across open air only. Lower keeps steps and rails distinct; higher bridges gaps and caps posts wider."));
			ImGui::SliderFloat(T(TKEY("volume_snow_rounding"), "Rounding"), &settings.VolumeSnowRounding, 0.0f, 32.0f, "%.0f u");
			if (auto _ttVoxRound = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("volume_snow_rounding_tooltip"), "How the snow rounds toward an edge: where half the neighbourhood is air the snow is this much lower, and it keeps falling smoothly past the edge into the lip. Separate from Overhang, which is where the lip is cut. On a ring whose voxels are larger than this the rounding cannot show, and there the far snow keeps its full slab instead."));
			ImGui::SliderFloat(T(TKEY("volume_max_slope"), "Max Slope"), &settings.VolumeSnowMaxSlopeDeg, 0.0f, 90.0f, "%.0f\xc2\xb0");
			if (auto _ttVoxSlope = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("volume_max_slope_tooltip"), "Surfaces steeper than this grow no volume snow. Read from the object's own surface normal, captured into the volume with it, so a wall is a wall whatever its stones do, and an underside never grows snow at any setting."));
			ImGui::SliderFloat(T(TKEY("volume_sky_exposure"), "Sky Exposure"), &settings.VolumeSkyExposurePct, 0.0f, 100.0f, "%.0f%%");
			if (auto _ttVoxSky = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("volume_sky_exposure_tooltip"), "How much the sky matters. At 100%% a spot that can see little sky grows little snow, so open ground piles deep and sheltered corners stay thin; at 0%% every surface grows the same depth."));
#if !SNOW_ALPHA_BUILD
			if (voxelOccupancyValid) {
				const auto d0 = VoxelDimsForLevel(0);
				const double occupiedPct = 100.0 * double(voxelOccupancy) / (double(d0.x) * d0.y * d0.z);
				WrapTextF("Occupied voxels, last frame: %u (%.2f%% of the cube)", voxelOccupancy, occupiedPct);
				if (auto _ttOcc = Util::HoverTooltipWrapper())
					ImGui::Text("%s", T(TKEY("voxel_occupancy_tooltip"), "The sanity number. Geometry is surfaces, so a few percent is plausible even in a busy town; tens of percent means the volume holds something other than surfaces and the picture cannot be trusted."));
			}
			ImGui::Checkbox(T(TKEY("voxel_show_rings"), "Colour Rings"), &showVolumeRings);
			if (auto _ttVoxRings = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("voxel_show_rings_tooltip"), "Tints the volume snow by the ring that drew it: green, blue, yellow, magenta, cyan, red from the nearest out. The dithered hand-over bands show as a speckled mix. Not saved."));
			ImGui::Checkbox(T(TKEY("voxel_force_rebuild"), "Force Volume Rebuild"), &debugForceVolumeRebuild);
			if (auto _ttVoxForce = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("voxel_force_rebuild_tooltip"), "Rebuilds every ring on its cadence even when nothing it reads has changed. Off, a ring whose window, captured objects, exclusions and settings have all held still for a memory's worth of rebuilds skips the rebuild entirely - the A/B for that gate. On while judging flicker with Dirty Bricks off, or the gate freezes the picture you are judging."));
			if (voxelSkipRate >= 0.0f) {
				WrapTextF("Volume rebuilds: idle (skipped) - %.0f%% of last 300 frames", voxelSkipRate * 100.0f);
				if (!voxelIdleSkipped && voxelIdleBlockers) {
					static constexpr const char* kBlockerNames[7] = { "window moved", "captures changed", "exclusions changed", "constants changed", "settling", "forced", "first build" };
					std::string why;
					for (int bit = 0; bit < 7; bit++)
						if (voxelIdleBlockers & (1u << bit))
							why += (why.empty() ? "" : ", ") + std::string(kBlockerNames[bit]);
					WrapTextDisabledF("Rebuilding now: %s", why.c_str());
				}
			}
			ImGui::Checkbox(T(TKEY("voxel_show_slice"), "Show Slice"), &showVoxelSlice);
			if (auto _ttVoxSlice = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("voxel_show_slice_tooltip"), "Draws a picture of what the volume holds."));
			if (showVoxelSlice) {
				ImGui::Checkbox(T(TKEY("voxel_xray"), "X-ray (see through the whole cube)"), &voxelSliceXray);
				if (auto _ttXray = Util::HoverTooltipWrapper())
					ImGui::Text("%s", T(TKEY("voxel_xray_tooltip"), "On: everything along the view direction is flattened into one image, so whole objects show as silhouettes - look at this first. Off: a single plane, one voxel thin, at the Offset - the only view that can show a roof with empty space beneath it."));
				const char* voxelSources[] = { "Objects (occupancy)", "Snow field (raw)", "Snow surface (field at Coverage)" };
				ImGui::Combo(T(TKEY("voxel_slice_source"), "Show"), &voxelSliceSource, voxelSources, IM_ARRAYSIZE(voxelSources));
				ImGui::SliderInt(T(TKEY("voxel_slice_level"), "Level"), &voxelSliceLevel, 0, std::clamp(settings.VolumeLevels, 1, (int)kVoxelMaxLevels) - 1);
				if (auto _ttVoxSrc = Util::HoverTooltipWrapper())
					ImGui::Text("%s", T(TKEY("voxel_slice_source_tooltip"), "Objects: what V0 captured. Snow field: the soft blob field grown from every object top (brighter = more snow). Snow surface: that field cut at Coverage - the shape the snow would take, before anything is drawn."));
				const char* voxelAxes[] = { "Top-down", "Side: looking north", "Side: looking east" };
				ImGui::Combo(T(TKEY("voxel_slice_axis"), "Plane"), &voxelSliceAxis, voxelAxes, IM_ARRAYSIZE(voxelAxes));
				if (!voxelSliceXray)
					ImGui::SliderFloat(T(TKEY("voxel_slice_offset"), "Offset from Camera"), &voxelSliceOffset, -1000.0f, 1000.0f, "%.0f u");
				// Long-form: the whole point of V0 is that this image can be
				// read without the plan open beside it.
				WrapText(T(TKEY("voxel_slice_hint"), "The square is the chosen level's window - the Finest level line further up gives the finest one's size - and the cross is its centre, which sits ahead of you by Forward Bias. Red = an object surface: bright when drawn this frame, dim as it fades. Each red block is one voxel of that level.\n\nStart with X-ray ON and a Side plane. A house should read as a box with a pitched roof, a rock as a blob. Uniform speckle with no shapes means the volume is wrong, and the number above will say so.\n\nThen X-ray OFF for the real test: slide the Offset until the plane cuts through a porch or covered walkway, and look for the roof as a line with EMPTY space beneath it and the floor as a second line below. A thin plane prints a drifting surface as dots rather than a line - that is normal."));
				const ImVec2 voxelImageTopLeft = ImGui::GetCursorScreenPos();
				if (voxelSliceTexture && voxelSliceTexture->srv) {
					ImGui::Image(voxelSliceTexture->srv.get(), { 512.0f, 512.0f });
					auto* draw = ImGui::GetWindowDrawList();
					const ImVec2 centre{ voxelImageTopLeft.x + 256.0f, voxelImageTopLeft.y + 256.0f };
					const ImU32 ink = IM_COL32(80, 220, 255, 220);
					draw->AddLine({ centre.x - 8.0f, centre.y }, { centre.x + 8.0f, centre.y }, ink, 1.5f);
					draw->AddLine({ centre.x, centre.y - 8.0f }, { centre.x, centre.y + 8.0f }, ink, 1.5f);
				}
			}
#endif
		}

		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("distant_snow"), "Distant Snow"), ImGuiTreeNodeFlags_Framed)) {
		if (auto _ttDs = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("distant_snow_tooltip"), "Snow on far terrain the game hasn't loaded: heights come from the worldspace heightmap (shipped with Community Shaders), and snow placement follows the game's own distant LOD textures — where the LOD is painted snowy, our snow appears. Loaded terrain always uses its real snow textures instead. The sliders here also set how far each snow system reaches; higher = more VRAM and GPU cost."));
		bool distantChanged = false;

		ImGui::Checkbox(T(TKEY("horizon_snow"), "Horizon Snow"), &settings.HorizonSnow);
		if (auto _ttHs = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("horizon_snow_tooltip"), "Recolors the game's distant LOD terrain with the shell's own snow material wherever its bake reads as snow, so snow appearance stays consistent from your feet to the horizon. The snow shell ends at the loaded-cell boundary and this takes over from there, out to the edge of the world."));

		ImGui::Checkbox(T(TKEY("lod_object_snow"), "Recolor Baked LOD Snow"), &settings.LODObjectSnow);
		if (auto _ttLodObj = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("lod_object_snow_tooltip"), "Distant objects the game draws as LOD without its projected-snow flags - snow drifts, snowy roads, piles - keep their own baked snow texture, which reads far brighter than the shell. This applies the Horizon Snow recolor to those pixels too, with the same detection slider below."));
		distantChanged |= ImGui::SliderFloat(T(TKEY("lod_snow_sensitivity"), "LOD Snow Detection"), &settings.LODSnowSensitivity, 0.0f, 1.0f, "%.2f");
		if (auto _ttLss = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("lod_snow_sensitivity_tooltip"), "How eagerly a distant LOD texture pixel counts as snow. The scale was widened: the old best-at-1.0 now sits near 0.5. Low = only bright white; high = pale gray rock starts counting too. The same setting drives the Horizon Snow recolor, so it decides where snow sits on the far terrain as well as how the shell reads it."));

		ImGui::SliderFloat(T(TKEY("range_trenches"), "Trenches"), &settings.RangeTrenchesM, 29.0f, 200.0f, "%.0f m");
		if (ImGui::IsItemDeactivatedAfterEdit())
			trenchRangeDirty = true;
		if (auto _ttRt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("range_trenches_tooltip"), "Deformation window radius: you carve anywhere inside it, NPCs out to NPC Trenches. Applying a change CLEARS existing trenches. NOT a GPU setting: GPU cost follows Deformation Map Resolution, not range. Texel detail is range divided by resolution, so a smaller range at the same resolution means sharper footprints over a shorter reach."));

		ImGui::SliderFloat(T(TKEY("range_actor_trenches"), "NPC Trenches"), &settings.RangeActorTrenchesM, 10.0f, 200.0f, "%.0f m");
		if (auto _ttRat = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("range_actor_trenches_tooltip"), "How far from the camera NPCs, creatures and corpses still carve trenches. Past it they leave no prints, and the work of following their bodies is skipped. You always carve. It never reaches past the Trenches range. Lower saves CPU time where many people or animals are on the move. Applies live."));

		ImGui::SliderFloat(T(TKEY("range_skins"), "Object Snow"), &settings.RangeSkinsM, kMinRangeSkinsM, kMaxRangeSkinsM, "%.0f m");
		if (auto _ttRk = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("range_skins_tooltip"), "Capture radius for snow skins on objects (rocks, cliffs, roofs); skins dissolve softly over the last ~29 m of it. Applies live."));

		ImGui::SliderFloat(T(TKEY("object_raster_reach"), "Object Height Map Range"), &settings.ObjectRasterReachM, 29.0f, 234.0f, "%.0f m");
		if (auto _ttOrr = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("object_raster_reach_tooltip"), "How far around you the top-down height map of the objects reaches. Inside it, snow thins under roofs, bridges and archways, road snow takes its shape and its trenches, footprints can be laid on objects, and Volume Snow finds what shelters it. Past it there is no shelter under cover and no footprints on objects, and road snow goes on from a coarser road-only map to the edge of the loaded cells. Keep it at or above the reach of Snow Trenches on Objects, or footprints on objects stop at this range. The map is a fixed 2048 texels however far it reaches, so this trades detail against range: a texel is about 7 world units at the default 100 m, 4 at 58 m. Costs nothing extra to raise; it coarsens the map near you instead."));

		if (distantChanged)
			shellDataDirty.store(true, std::memory_order_release);

		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("menu_blood_splatter"), "Blood Splatter"), ImGuiTreeNodeFlags_Framed)) {
		ImGui::Checkbox(T(TKEY("blood_on_snow"), "Blood on Snow"), &settings.BloodOnSnow);
		if (auto _ttBlood = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("blood_on_snow_tooltip"), "Every blood mod paints the ground, which the snow covers, so a kill in a snowfield left nothing to see. On, the game's blood decals and pool quads (vanilla, Enhanced Blood Textures, Sanguine Symphony, Dynamic Bloodpool Framework - anything whose texture is a blood texture) are copied the moment they appear, and the landscape snow shows them: within 100 m of you as the marks themselves, droplets and streaks at a quarter of a unit, painted on the snow; farther out, and where a very large battle runs past what that layer can hold, as softer copies of them, painted the same way. On snow lying on objects (the recolored projected snow) the game's own decals are put back on top of the snow instead. Wounds on bodies and sprays on walls stay the game's own. Marks last until snowfall buries them or the map scrolls away; digging finds the game's stain on the ground beneath."));
		ImGui::SliderFloat(T(TKEY("blood_intensity"), "Blood Intensity"), &settings.BloodIntensity, 0.0f, 2.0f, "%.2f");
		if (auto _ttBloodI = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("blood_intensity_tooltip"), "How much blood each mark deposits and how strongly it stains the landscape shell. Object snow (the recolored projected snow) carries the game's own decal on top instead and is not stained. 1 takes the textures as authored; 2 doubles it."));
		ImGui::SliderFloat(T(TKEY("blood_burial"), "Snowfall Burial"), &settings.BloodBurial, 0.02f, 1.0f, "%.2f refills");
		if (auto _ttBloodB = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("blood_burial_tooltip"), "How much snowfall hides a mark for good, measured in trench refills: the same clock that fills a footprint back in. 0.15 = buried by a seventh of the snow that fills a trench, so a blizzard scrubs a battlefield in minutes while clear cold weather keeps it all session."));
		ImGui::SliderFloat(T(TKEY("blood_age_hours"), "Drying Time"), &settings.BloodAgeHours, 0.25f, 24.0f, "%.1f h");
		if (auto _ttBloodA = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("blood_age_hours_tooltip"), "Game hours over which a fresh mark dries: the wet gloss goes and the red turns maroon."));
		ImGui::SliderFloat(T(TKEY("blood_sheen"), "Wet Sheen"), &settings.BloodSheen, 0.0f, 1.0f, "%.2f");
		if (auto _ttBloodS = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("blood_sheen_tooltip"), "Gloss of fresh blood, 0 = matte like the snow around it."));
		ImGui::SliderFloat(T(TKEY("blood_spread_seconds"), "Appear Time"), &settings.BloodSpreadSeconds, 0.0f, 6.0f, "%.1f s");
		if (auto _ttBloodSp = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("blood_spread_seconds_tooltip"), "A fresh mark does not appear all at once: it grows from its dense core to its thin fringe over this long. 0 = the whole mark at once."));
		if (bloodTilesFailed)
			WrapTextColoredF({ 1.0f, 0.35f, 0.35f, 1.0f }, "%s", T(TKEY("blood_tiles_status_failed"), "The detailed blood marks are NOT RUNNING: a texture or shader failed - see CommunityShaders.log. The blood map carries on alone."));
		if (bloodShadersFailed)
			WrapTextColoredF({ 1.0f, 0.35f, 0.35f, 1.0f }, "%s", T(TKEY("blood_status_failed"), "NOT RUNNING: a blood shader failed to compile - see CommunityShaders.log."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("snow_refill"), "Snow Refill"), ImGuiTreeNodeFlags_Framed)) {
		if (auto _ttRefillTree = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("snow_refill_tooltip"), "How compressed snow recovers, and how deep the layer grows while it snows."));
		ImGui::Checkbox(T(TKEY("refill_only_snowing"), "Refill Only While Snowing"), &settings.RefillOnlyWhenSnowing);
		if (auto _ttRefillSnow = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("refill_only_snowing_tooltip"), "Compressed snow only recovers while the current weather is snowing, faster in denser snowfall. Trails and trenches persist through clear weather. Off: snow recovers at the baseline rate in any weather."));

		ImGui::SliderFloat(T(TKEY("refill_rate"), "Snow Refill Rate"), &settings.RefillRateMultiplier, 0.0f, 10.0f, "%.1fx");
		if (auto _ttRefill = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("refill_rate_tooltip"), "Multiplier on the snowfall-driven refill rate. At 1.0x, typical snowfall recovers compressed snow in about 12 minutes. 0 disables refilling."));

		ImGui::SeparatorText(T(TKEY("snow_accumulation"), "Snow Accumulation"));
		if (auto _ttAccumCat = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("snow_accumulation_tooltip"), "The snow layer deepens while it snows and settles back when it stops, so a long storm leaves the world deeper than it found it. Landscape snow only - snow sitting on objects keeps its fixed depth."));

		ImGui::Checkbox(T(TKEY("enable_accumulation"), "Snow Accumulation"), &settings.EnableSnowAccumulation);
		if (auto _ttAccumEnable = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("enable_accumulation_tooltip"), "Let snowfall deepen the snow. Off holds every kind of ground at its set depth whatever the weather does, which is how the mod behaved before this existed - useful for comparing the two."));

		ImGui::Checkbox(T(TKEY("recolored_snow_accumulates"), "Recolored Snow Accumulates"), &settings.RecoloredSnowAccumulates);
		if (auto _ttAccumRecolor = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("recolored_snow_accumulates_tooltip"), "The snow on rocks and buildings creeps out from its painted patches as it snows and draws back as it melts: Edge Lump Reach runs from 0 to 1 over Accumulation Time, back over Melt Time and Accumulated Snow Fade, whatever the Accumulation Peak. Off, the Edge Lump Reach slider is used as set."));

		ImGui::Checkbox(T(TKEY("persist_accumulation"), "Remember Snow Accumulation"), &settings.PersistAccumulation);
		if (auto _ttAccumPersist = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("persist_accumulation_tooltip"), "How deep the layer has grown is written to the save and comes back with it, so loading in the middle of a week-long winter finds the world as deep as you left it. Off, every load starts at the authored depth and the layer has to build again from whatever the sky is doing - which is also what to use when comparing, since the depth then depends only on the weather since the load."));

		ImGui::SliderFloat(T(TKEY("accumulation_peak"), "Accumulation Peak"), &settings.AccumulationPeak, 1.0f, 2.0f, "%.2fx");
		if (auto _ttAccumPeak = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("accumulation_peak_tooltip"), "How deep the snow gets after a long storm, as a multiple of its normal depth. Every kind of ground grows by the same proportion, so paths and roads stay lower than the fields around them - in fact the gap between them widens as it snows, which keeps a road readable. 1.00x means snowfall never deepens anything."));

		ImGui::SliderFloat(T(TKEY("accumulation_hours"), "Accumulation Time"), &settings.AccumulationHours, 1.0f, 10.0f, "%.0f game hours");
		if (auto _ttAccumHours = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("accumulation_hours_tooltip"), "How long heavy snowfall takes to build the layer from its normal depth up to the peak. Lighter snow takes proportionally longer, so a thin flurry barely moves it."));

		ImGui::SliderFloat(T(TKEY("accumulation_melt_hours"), "Melt Time"), &settings.AccumulationMeltHours, 1.0f, 10.0f, "%.0f game hours");
		if (auto _ttAccumMelt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("accumulation_melt_hours_tooltip"), "How long clear weather takes to settle the layer back down from the peak. By default the same as Accumulation Time; set it longer and a snowy stretch stays deep between storms."));

		ImGui::SliderFloat(T(TKEY("accumulation_fade"), "Accumulated Snow Fade"), &settings.AccumulationFadeDays, 0.0f, 14.0f, "%.0f days");
		if (auto _ttAccumFade = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("accumulation_fade_tooltip"), "A slow settling that runs in all weather, snowfall included, so the world always finds its way back to its normal depth instead of climbing for ever through an endless winter. Because it never stops, it also makes clear weather settle somewhat faster than Melt Time alone would. 0 turns it off and leaves clear weather as the only thing that brings the snow back down."));

		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("undulation"), "Snow Undulation"), ImGuiTreeNodeFlags_Framed)) {
		if (auto _ttUnd = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("undulation_tooltip"), "Wind-worked waves in deep snow. They fade out automatically over thin cover, class borders and carved trench floors."));
		ImGui::SliderFloat(T(TKEY("undulation_strength"), "Undulation Strength"), &settings.UndulationStrength, 0.0f, 8.0f, "%.1f units");
		if (auto _ttUs = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("undulation_strength_tooltip"), "Wave height. 0 flattens deep snow into a smooth sheet."));

		ImGui::SliderFloat(T(TKEY("undulation_spacing"), "Undulation Spacing"), &settings.UndulationSpacing, 0.5f, 4.0f, "%.1fx");
		if (auto _ttUsp = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("undulation_spacing_tooltip"), "Stretches the wave pattern: larger = broader, calmer dunes instead of a spike carpet."));


		ImGui::SliderFloat(T(TKEY("parallax_depth"), "Parallax Depth"), &settings.ParallaxDepth, 0.0f, 2.0f, "%.2fx");
		if (auto _ttPd = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("parallax_depth_tooltip"), "Parallax occlusion mapping on the landscape shell: marches the view ray through the snow texture's displacement map and shades from where it hits, so grain occludes grain and the surface reads as thick instead of merely lit. It moves no vertices, and the depth it resolves is by construction the depth of the grain being drawn. A multiplier on the PBR config's displacementScale - 1.0 is exactly the slab depth PBR ground gets. 0 skips the march. It also sets how strongly the trench detail shades itself from the sun: 0 turns that off, 1 or more gives it in full."));

		ImGui::SliderFloat(T(TKEY("parallax_shadow_strength"), "Parallax Shadow"), &settings.ParallaxShadowStrength, 0.0f, 2.0f, "%.2fx");
		if (auto _ttPss = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("parallax_shadow_strength_tooltip"), "Self-shadowing of the snow's own grain, the same term PBR ground receives from Extended Materials: four taps along the sun through the displacement map, so the micro-relief casts into itself under low sun instead of reading flat. Needs the PBR snow set's _p map. 0 skips the taps entirely (and is the A/B for their cost). It scales the trench detail's own sun shadow the same way: 0 turns that off too."));

		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("class_depths"), "Snow Depth by Texture Class"), ImGuiTreeNodeFlags_Framed)) {
		if (auto _ttClasses = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("class_depths_tooltip"), "Starting height for each snow texture family (classified by the vanilla LTEX filenames every retexture mod overrides). This is the DEFAULT a texture uses until it is given its own value below. Negative values submerge the shell below the surface. Retunes live from cached data."));
		ImGui::SeparatorText(T(TKEY("roads_group"), "Roads"));
		ImGui::Checkbox(T(TKEY("city_shells"), "Landscape And Road Snow In Cities"), &settings.CityShells);
		if (auto _ttCity = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("city_shells_tooltip"), "Draws the landscape shell and road snow inside the walled cities (their own worldspaces). Off, cities keep object snow only; the ground there stays as the game draws it."));

		ImGui::SliderFloat(T(TKEY("road_meshes_depth"), "Road Meshes"), &settings.RoadMeshesDepth, 0.0f, 64.0f, "%.0f units");
		if (auto _ttRoad = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("road_meshes_depth_tooltip"), "Snow layer on road meshes; bridges are ordinary objects. Kept below the surrounding snow classes so the road's course stays readable through the snowfield."));

		ImGui::SeparatorText(T(TKEY("landscape_class_group"), "Landscape Texture Class"));
		bool classDepthsChanged = false;
		for (uint32_t classI = 0; classI < kSnowClassCount; ++classI)
			classDepthsChanged |= ImGui::SliderFloat(kSnowClasses[classI].label, &settings.SnowClassDepths[classI], -20.0f, 64.0f, "%.0f units");
		if (classDepthsChanged) {
			RefreshLandTextureDepths();
			shellDataDirty.store(true, std::memory_order_release);
		}
		ImGui::SliderFloat(T(TKEY("slope_drape"), "Slope Drape"), &settings.SlopeDrape, 0.0f, 1.0f, "%.2f");
		if (auto _ttDrape = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("slope_drape_tooltip"), "How the ground snow stands off steep ground. At 0 it is always lifted straight up, so on a near-vertical face the cover thins to almost nothing and the rock shows through in diagonal gaps. At 1 the lift turns toward the surface as the ground steepens (from 30 degrees, fully by 60), so the layer keeps its full depth in front of the face - the way snow plasters a mountainside rather than only sitting on top of it. Snow on objects is not affected."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("texture_depths"), "Snow Depth by Landscape Texture"), ImGuiTreeNodeFlags_Framed)) {
		if (auto _ttTextures = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("texture_depths_tooltip"), "Every landscape texture the game has loaded this session, each with its own shell height. A texture starts at its family's default above; moving it here pins it to its own value, saved by texture path so load order cannot rebind it. Use this to give snow to textures the families lump in with bare ground (snowy stone, frozen marsh), or to hold one texture back."));

		std::vector<LandTextureEntry> textures;
		{
			const std::shared_lock lock(landTextureMutex);
			textures = landTextures;
		}

		ImGui::InputText(T(TKEY("texture_filter"), "Filter"), &textureFilter);
		std::string filter = textureFilter;
		std::transform(filter.begin(), filter.end(), filter.begin(),
			[](unsigned char c) { return (char)std::tolower(c); });

		// Snow families first (table order), then alphabetical, so the
		// textures worth tuning are not buried under the bare ground ones.
		std::vector<uint16_t> order(textures.size());
		std::iota(order.begin(), order.end(), (uint16_t)0);
		std::sort(order.begin(), order.end(), [&](uint16_t a, uint16_t b) {
			if (textures[a].classIndex != textures[b].classIndex)
				return textures[a].classIndex < textures[b].classIndex;
			return textures[a].label < textures[b].label;
		});

		WrapTextF("%zu textures loaded this session", textures.size());

		for (uint16_t textureI : order) {
			const auto& entry = textures[textureI];
			if (!filter.empty() && entry.path.find(filter) == std::string::npos)
				continue;

			ImGui::PushID((int)textureI);
			float depth = entry.depth;
			if (ImGui::SliderFloat(entry.label.c_str(), &depth, -20.0f, 64.0f, "%.0f units")) {
				SetLandTextureOverride(entry.path, depth);
				shellDataDirty.store(true, std::memory_order_release);
			}
			if (auto _ttEntry = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s\nFamily: %s (%.0f units)", entry.path.c_str(),
					kSnowClasses[entry.classIndex].label, settings.SnowClassDepths[entry.classIndex]);
				if (entry.overridden)
					ImGui::Text("%s", T(TKEY("texture_pinned"), "Pinned to its own value; Reset hands it back."));
				else if (entry.shipped)
					ImGui::Text("%s", T(TKEY("texture_shipped"), "Ships with its own default, so the family slider does not reach it."));
			}
			if (entry.overridden) {
				ImGui::SameLine();
				if (ImGui::SmallButton(T(TKEY("texture_reset"), "Reset"))) {
					SetLandTextureOverride(entry.path, std::nullopt);
					shellDataDirty.store(true, std::memory_order_release);
				}
			}
			ImGui::PopID();
		}

		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("snow_trenches"), "Snow Trenches"), ImGuiTreeNodeFlags_Framed)) {
		if (auto _ttTd = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("snow_trenches_tooltip"), "Everything about the ground being walked through: how long a trench is remembered, how wide and how sharply it cuts, and the look of the disturbed snow around it. Untouched snow is never affected."));

		// Promoted from Debugging Options once the tile-dispatch work made
		// it a real performance lever. Applies like a Trenches-range
		// change: the map clears and the trench store re-injects what it
		// remembers.
		{
			static const uint kMapDims[] = { 1024u, 2048u, 4096u };
			int dimIndex = settings.DeformMapResolution <= 1024u ? 0 : (settings.DeformMapResolution >= 4096u ? 2 : 1);
			if (ImGui::Combo(T(TKEY("map_resolution"), "Deformation Map Resolution"), &dimIndex, "1024\0" "2048\0" "4096\0")) {
				settings.DeformMapResolution = kMapDims[dimIndex];
				deformMapDimDirty = true;
			}
			if (auto _ttRes = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("map_resolution_tooltip"), "Resolution of the map every trench, melt mark and berm lives in. The main performance dial for this section: cost scales with the square of it, so each step down roughly quarters the trench passes' GPU time. Detail follows the Trenches range too - at the default range, 2048 gives roughly 12 cm per texel, 1024 roughly 24 cm (footprints soften but trails stay). 4096 halves the texel again and costs roughly four times 2048's trench-pass time and video memory; the High preset uses it. Changing it clears the map; remembered trenches are re-injected from the store."));
			if (ImGui::Checkbox(T(TKEY("vram_aware_map"), "VRAM-Aware Map Size"), &settings.VRAMAwareTrenchMap) && !settings.VRAMAwareTrenchMap && deformMapVramCap) {
				deformMapVramCap = 0;
				deformMapDimDirty = true;
			}
			if (auto _ttVram = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("vram_aware_map_tooltip"), "When the graphics card's free video memory budget gets smaller than what 4096 costs over 2048 (about 190 MB), the map runs at 2048 for the rest of the session: past the budget the driver moves textures to system memory and frames stall. The resolution above keeps its value. The map clears once; remembered trenches are re-injected from the store."));
			if (deformMapVramCap && deformMapDim < settings.DeformMapResolution)
				ImGui::TextDisabled("%s %u", T(TKEY("vram_aware_map_active"), "Video memory over budget this session, map running at"), deformMapDim);
		}

		{
			static const uint kPrintDims[] = { 512u, 1024u, 2048u };
			int printIndex = settings.ObjectTrenchResolution <= 512u ? 0 : (settings.ObjectTrenchResolution >= 2048u ? 2 : 1);
			if (ImGui::Combo(T(TKEY("object_trench_reach"), "Snow Trenches on Objects"), &printIndex, "11 m\0" "22 m\0" "44 m\0"))
				settings.ObjectTrenchResolution = kPrintDims[printIndex];
			if (auto _ttObjectTrench = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("object_trench_reach_tooltip"), "How far around you footprints in the snow on objects - roofs, floorboards, rocks, logs, bridges - are kept: 11, 22 or 44 m. They are always on. Like trenches in the ground they refill while it snows, and with Remember Trenches on they are kept when you walk away and saved with the game. The print map takes 2, 8 or 32 MB of video memory. Changing it keeps remembered footprints and drops the last moments not yet stored."));
		}

		ImGui::Checkbox(T(TKEY("tessellation"), "Tessellate Trenches"), &settings.Tessellation);
		if (auto _ttTess = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("tessellation_tooltip"), "Adds vertex density to the ground snow and the road trench patch near the camera, keyed off the deformation map, so carves resolve as smooth walls instead of following the ground's own triangles. This is what trench smoothness actually depends on. Off leaves every trench as angular as the triangles beneath it."));

		if (ImGui::Checkbox(T(TKEY("persist_trenches"), "Remember Trenches"), &settings.PersistTrenches) && !settings.PersistTrenches)
			ClearTrenchStore("the Remember Trenches toggle");
		if (auto _ttPersist = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("persist_trenches_tooltip"), "Trenches survive leaving the area. Snow deformation is drawn in a window that follows the camera, and without this everything outside it is discarded: walk a few hundred metres away and your trail is gone when you come back. On, departing ground is kept in a sparse store, put back on return, and written to the save so it is still there after a reload. Off restores the old behaviour and frees the store."));

		if (settings.PersistTrenches) {
			ImGui::SliderFloat(T(TKEY("trench_memory"), "Trench Memory"), &settings.TrenchMemoryMB, 0.02f, 8.0f, "%.2f MB");
			if (auto _ttMemory = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("trench_memory_tooltip"), "How much the world is allowed to remember, measured as the space it will take up in your save. Past it, the ground you visited longest ago is forgotten first. 1 MB is around three thousand patches of trodden ground - far more than snowfall usually leaves standing, so weather normally clears old trenches long before this limit matters and it sits here as a backstop for weather that never comes. Note this is the SAVE cost: trench data packs down more than forty times over, so it takes far more RAM than this while you play, and every save file carries its own copy."));

			ImGui::SliderFloat(T(TKEY("stored_trench_fade"), "Stored Trench Fade"), &settings.StoredTrenchFadeDays, 0.0f, 30.0f, "%.0f days");
			if (auto _ttFade = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("stored_trench_fade_tooltip"), "How long a remembered trench lasts with no snowfall at all. Snowfall does the real erasing, at the same rate it erases the ground in front of you, so a trench behaves the same whether or not you are looking at it - this is the slow floor underneath that, so a world where it never snows still forgets eventually instead of remembering for ever. 0 turns the floor off and leaves snowfall as the only thing that clears stored trenches."));
		}

		ImGui::SeparatorText(T(TKEY("trench_detail_group"), "Trench Detail"));
		if (auto _ttDetail = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("trench_detail_group_tooltip"), "The shape and surface of disturbed snow: the raised berm along trench edges, the broken snow in and around trenches and the broken rim. Road trenches wear all of it; snow on objects takes the berm."));

		ImGui::SliderFloat(T(TKEY("berm_height"), "Berm Height"), &settings.BermHeight, 0.0f, 1.0f, "%.2fx");
		if (auto _ttBh = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("berm_height_tooltip"), "Height of the pushed-aside snow ridge along trench edges, as a fraction of the local snow depth. 0 removes the berm. On objects the same value shades a ridge rather than raising one."));

		ImGui::SliderFloat(T(TKEY("berm_steepness"), "Berm Steepness"), &settings.BermSteepness, 0.0f, 2.0f, "%.2f");
		if (auto _ttBs = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("berm_steepness_tooltip"), "How tightly the berm hugs the trench, at the same crest height. 1.00 = the default apron, reaching about 40 units out; 0 spreads it twice as wide and gentle, 2 packs it into half the width - a steep ridge."));

		ImGui::SliderFloat(T(TKEY("trench_detail_relief"), "Detail Relief"), &settings.TrenchDetailRelief, 0.0f, 2.0f, "%.2fx");
		if (auto _ttTdRelief = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("trench_detail_relief_tooltip"), "Height of the broken snow in and around trenches, on the ground and on roads. It is drawn with parallax occlusion: each pixel follows its view ray into the clods, crumbs and slabs, so nearer ones hide what lies behind them and shade it from the sun, while the snow keeps your snow texture's colour, sparkle and lighting. It eases off on thin snow and on trampled floors (Flattening). Disturbed snow wears the powder detail in every weather; where something hits it hard - a sprint, a dodge, a landing, a body knocked down, lightning, a Force shout - it turns to the impact detail, the harder the more, and rises up to 1.5x. Snow fire has melted wears the impact detail too. 1 = as authored, 0 = off."));

		ImGui::SliderFloat(T(TKEY("trench_detail_roughness"), "Detail Roughness"), &settings.TrenchDetailRoughness, 0.3f, 1.5f, "%.2fx");
		if (auto _ttTdRough = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("trench_detail_roughness_tooltip"), "Snow roughness where the detail applies, as a multiplier. Lower is glossier, for wet snow; 1 leaves the snow set's own roughness."));

		ImGui::SliderFloat(T(TKEY("trench_detail_coverage"), "Detail Coverage"), &settings.TrenchDetailCoverage, 0.0f, 2.5f, "%.2f");
		if (auto _ttTdCover = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("trench_detail_coverage_tooltip"), "How far across a trench the detail reaches. 0 = the trench floor only, 1 = the floor and walls to just past the rim, 2 = out over the berm, 2.5 = the berm's whole apron. The edge fades softly at every setting and every snow depth, and the detail fades out where a trench meets an object."));

		ImGui::SliderFloat(T(TKEY("object_print_rim"), "Object Print Rim"), &settings.ObjectPrintRimWidth, 0.0f, 15.0f, "%.1f units");
		if (auto _ttPrintRim = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("object_print_rim_tooltip"), "How far past a footprint's edge the snow on objects - roofs, floorboards, rocks, logs - is broken up, in units. The trench detail (Detail Coverage, at twice Detail Relief) runs this far around each print and fades out, strongest at the print's edge. 0 = only the print's own edge."));

		ImGui::SliderFloat(T(TKEY("impact_reach"), "Impact Reach"), &settings.ImpactReach, 0.5f, 4.0f, "%.2fx");
		if (auto _ttImpactReach = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("impact_reach_tooltip"), "How far the impact detail spreads around a hard hit - a sprint, a dodge, a landing, a body knocked down - as a share of what hit the snow. At 1 a foot's impact covers its own print and a thin ring around it, and a body knocked down an area about its own size; 2 is twice as wide. The snow it throws is Speed Spoil's. Applies to new impacts."));

		ImGui::Checkbox(T(TKEY("speed_spoil"), "Speed Spoil"), &settings.SpeedSpoil);
		if (auto _ttSpoil = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("speed_spoil_tooltip"), "A foot leaving its print faster than a run throws snow back from it in a wedge - the faster, and the heavier the creature, the longer and stronger - and the impact detail shows that thrown snow, past the trench too, until snowfall buries it. It carves nothing; it shows which way someone was running. On the ground and on roads."));
		if (settings.SpeedSpoil) {
			ImGui::SliderFloat(T(TKEY("speed_spoil_size"), "Spoil Size"), &settings.SpeedSpoilSize, 0.1f, 2.0f, "%.2fx");
			if (auto _ttSpoilSize = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("speed_spoil_size_tooltip"), "How far the thrown snow reaches, length and width together. At 1 a sprint throws it about a metre back from each print; 0.5 is half that. Applies to new wedges."));
			ImGui::TextDisabled("%s %llu", T(TKEY("speed_spoil_count"), "Wedges thrown since load:"), static_cast<unsigned long long>(speedSpoilWedges));
		}

#if !SNOW_ALPHA_BUILD
		ImGui::Checkbox(T(TKEY("trench_detail_view"), "Detail View"), &debugTrenchDetailView);
		if (auto _ttTdView = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("trench_detail_view_tooltip"), "Paints the snow orange where the trench detail applies, over grey shading, on the ground and on roads, to judge Coverage at a glance."));
#endif
		if (trenchDetailAttempted && !trenchDetailNormalSRV)
			ImGui::TextDisabled("%s", T(TKEY("trench_detail_missing"), "Trench detail textures not found: Shaders\\SnowDeformation\\TrenchDetailN.dds and TrenchDetailH.dds."));

		ImGui::SliderFloat(T(TKEY("rim_lip"), "Rim Lip"), &settings.RimLip, 0.0f, 0.3f, "%.2f");
		if (auto _ttLip = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("rim_lip_tooltip"), "The trench rim rolls UP slightly before it drops - the cornice look of cut snow. Height as a fraction of local snow depth; deep snow only (shallow dimples stay smooth). 0 = off."));

		ImGui::SliderFloat(T(TKEY("rim_teeth"), "Rim Teeth"), &settings.RimTeeth, 0.0f, 1.0f, "%.2f");
		if (auto _ttTeeth = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("rim_teeth_tooltip"), "Breaks the trench edge into irregular teeth and blocks instead of a clean curve, from a ragged noise fixed to the ground. Deep snow only. Too high eats the trench's readable width - back off if trails start looking chewed. 0 = off."));

		ImGui::SeparatorText(T(TKEY("snow_condition_group"), "Snow Condition"));

		ImGui::SliderFloat(T(TKEY("snow_resistance"), "Snow Resistance"), &settings.SnowResistance, 0.0f, 100.0f, "%.0f %%");
		if (auto _ttResist = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("snow_resistance_tooltip"), "How much of a body's push the snow holds back. Feet stand on the ground beneath the snow, so at 0% every step cuts to the bottom and anything low - a long skirt, a hand, a blade - cuts as deep as it reaches. At 50% one pass carves half of that: legs sink into the snow and the trench behind stays shallow. Every new pass over the same snow carves the same share of what is left, so busy paths sink deeper the more they are walked. Dropped items, spells and melt keep their own depths. Affects new trenches only."));

		ImGui::Checkbox(T(TKEY("creature_sink"), "Creatures Sink by Weight"), &settings.CreatureSink);
		if (auto _ttCreatureSink = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("creature_sink_tooltip"), "Creatures lighter than you sink only part way into the snow. Where the snow is firm enough to carry them, a hare or a fox stands up to half way up it and leaves a shallower trench; in loose new snow they sink further, but never past their belly, and never into something low overhead. Your own weight is the measure: people sink as you do, and anything as heavy as you or heavier stands on the ground under the snow. A creature that dies settles into the snow. Weight comes from each creature's own collision shapes, so modded creatures work too. The same weight sets how hard a creature hits the snow: a horse's gallop or landing throws more broken snow than a fox's. Off, every creature stands on the ground and hits the snow as you do."));

		ImGui::SliderFloat(T(TKEY("track_ageing"), "Track Ageing"), &settings.TrackAgeing, 0.0f, 2.0f, "%.2f");
		if (auto _ttTrackAge = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("track_ageing_tooltip"), "How fast tracks age. An old track's walls round off, shallower and wider, its rim loses its teeth and lip, its berm settles, and new snow covers the broken snow and the dark wet print. Snowfall ages tracks fastest: at 1.00 an hour of it does what a day of calm cold does. Walking an old track again makes it fresh where your feet reach the floor. Ages are kept in saves. 0 = tracks never age."));

		ImGui::SliderFloat(T(TKEY("snow_climate"), "Snow Climate"), &settings.SnowClimateStrength, 0.0f, 1.0f, "%.2f");
		if (auto _ttClimate = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("snow_climate_tooltip"), "How much the kind of snow decides what a trench looks like. The snow's temperature comes from how snowy the land is, the weather, the sun and the height. At 1.00, cold powder leaves holes with no ridge and loose snow fallen back into them; settled snow just below freezing keeps the full ridge, lip and teeth; wet snow leaves dark prints with crisp walls. Berm Height, Rim Lip and Rim Teeth are the most the snow can give. At 0 the look is the fixed one set by the sliders and Fresh Snow Softness. Debugging Options > Snow Condition shows what the snow asks for."));

		ImGui::Checkbox(T(TKEY("wet_snow_ice"), "Wet Snow and Ice"), &settings.WetSnowAndIce);
		if (auto _ttWetIce = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("wet_snow_ice_tooltip"), "Snow soaked by rain or a thaw turns greyer and stops sparkling, and the packed snow of prints and paths reads as slush. When the cold comes back, the water held in packed snow freezes: trampled paths and roads glaze over, smooth, glossy and faintly blue, until a thaw or fresh snow ends the ice. The rest of the snow takes a faint crust of the same glaze. Higher ground freezes first. Follows Snow Climate. Off, only the dark wet print remains."));

		ImGui::SliderFloat(T(TKEY("fresh_snow_softness"), "Fresh Snow Softness"), &settings.FreshSnowSoftness, 0.0f, 1.0f, "%.2f");
		if (auto _ttFresh = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("fresh_snow_softness_tooltip"), "New snow is loose: it cannot hold a cut wall, and it packs down under a boot instead of being thrown aside. While it snows, and for a while after, the ridge beside trenches shrinks and the rim loses its lip and teeth; trenches cut by distant creatures, spells and shouts also get sloped sides instead of sharp walls. As the snow settles over the following days, the look you set with Berm Height, Rim Lip and Rim Teeth comes back and those walls sharpen again; a trench keeps the walls it was cut with. 0 = off, the snow is always firm. Snow Climate takes over from this as it rises: at 1.00 fresh snow acts through its stickiness and density instead."));

		ImGui::SliderFloat(T(TKEY("fresh_snow_settle"), "Fresh Snow Settles In"), &settings.FreshSnowSettleHours, 6.0f, 168.0f, "%.0f game hours");
		if (auto _ttSettle = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("fresh_snow_settle_tooltip"), "How long clear weather takes to turn loose new snow fully firm. About two game hours of heavy snowfall makes it fully fresh again; lighter snow takes longer."));

		ImGui::SeparatorText(T(TKEY("trampled_group"), "Trampled Ground"));

		ImGui::SliderFloat(T(TKEY("trampled_flattening"), "Flattening"), &settings.TrampledFlattening, 0.0f, 1.0f, "%.2fx");
		if (auto _ttTrampleFlat = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("trampled_flattening_tooltip"), "Snow walked over again and again is packed down, and the broken snow on the trench floor is walked flatter: this multiplies Detail Relief on floors deeper than one pass leaves them, easing in the deeper the traffic has packed them. At 0.50x a Detail Relief of 1 becomes 0.5. The berms and a trail walked once are never touched. 1.00x = off."));



		ImGui::SeparatorText(T(TKEY("dropped_items_group"), "Dropped Items"));
		ImGui::Checkbox(T(TKEY("item_sink"), "Enable Dynamic Object Sink Depth"), &settings.ItemSink);
		if (auto _ttItemSink = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("item_sink_tooltip"), "A dropped item rests in the snow at a depth set by its weight and size: a ring stays on the surface, a shield goes to the bottom of the trench it cuts. Off, every item falls to the ground under the snow, as the game places it. Items stay where physics left them either way, so picking them up is unchanged."));
		ImGui::BeginDisabled(!settings.ItemSink);
		ImGui::SliderFloat(T(TKEY("item_embed"), "Dropped Item Sink"), &settings.ItemEmbedPercent, 0.0f, 100.0f, "%.0f %%");
		if (auto _ttEmbed = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("item_embed_tooltip"), "How far a heavy dropped item presses into the floor of the trench it lands in, as a share of its own thickness. Shape decides how much of this an item takes: rounded items (armor, buckets, boots) take all of it, flat ones (shields, swords, books) almost none, so they stay in view. Light items rest on the snow whatever this is. 0 = every item rests on its trench floor."));
		ImGui::SliderFloat(T(TKEY("item_min_roundness"), "Minimum Roundness"), &settings.ItemMinRoundness, 0.0f, 1.0f, "%.2f");
		if (auto _ttRound = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("item_min_roundness_tooltip"), "How rounded an item must be before it presses into its trench floor at all. Roundness is an item's thickness against the width of its largest side: a greatsword is about 0.08, a book 0.15, a shield 0.24, boots 0.6, a bucket 0.9. Items at or under this value rest on the floor; the share grows from here to the full Dropped Item Sink at 0.5. Raise it if flat items still disappear, lower it to let them sink."));
		ImGui::SliderFloat(T(TKEY("item_snow_grip"), "Snow Grip"), &settings.ItemSnowGripPercent, 0.0f, 100.0f, "%.0f %%");
		if (auto _ttGrip = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("item_snow_grip_tooltip"), "Snow packs under what drops into it and stops it rolling. The deeper an item is in the snow and the slower it is travelling, the harder it is held: dropped straight into deep snow it stays where it lands, thrown hard it still slides until it slows, and in shallow snow it rolls almost as it would on bare ground. 0 = the game's own physics."));
		ImGui::EndDisabled();

		ImGui::PushID("snow_trenches");
		if (ImGui::TreeNodeEx(T(TKEY("menu_advanced"), "Advanced"))) {
			ImGui::SliderFloat(T(TKEY("snow_slumping"), "Snow Slumping"), &settings.SlumpRate, 0.0f, 1.0f, "%.2f");
			if (auto _ttSlump = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("snow_slumping_tooltip"), "Snow dug away on BOTH sides loses its support and settles about halfway into a low uneven bump, so heavy traffic reads as one churned channel instead of a comb of full-height fins. Trench walls and open snow never move. 0 = off; higher = faster settling."));

			ImGui::Checkbox(T(TKEY("no_carve_floating"), "Floating Actors Leave No Trench"), &settings.NoCarveFloatingActors);
			if (auto _ttFloat = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("no_carve_floating_tooltip"), "Far field only. Inside the contact window (about 44 m) an actor carves by its render mesh, and anything that hovers reaches no snow by construction; this gate covers the bone path beyond it. Stops things that never touch the ground from digging it: atronachs, wisps, ghosts, anything that hovers. Nothing is named - an actor is judged by whether its own lowest part ever comes down to its footing, so modded levitators are covered too."));

			ImGui::SliderFloat(T(TKEY("floating_band"), "Floating Actor Clearance"), &settings.FloatingActorBand, 4.0f, 80.0f, "%.0f units");
			if (auto _ttFloatBand = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("floating_band_tooltip"), "Far field only (the bone path beyond the contact window, about 44 m). How far an actor's lowest part may sit above its footing and still count as standing on it. Lower values catch things that only just hover, at the risk of dropping a normal creature's tracks mid-stride."));

			{
				std::string incorporealModes;
				incorporealModes += T(TKEY("incorporeal_off"), "Off");
				incorporealModes += '\0';
				incorporealModes += T(TKEY("incorporeal_translucent"), "See-through bodies");
				incorporealModes += '\0';
				incorporealModes += T(TKEY("incorporeal_flag"), "Actors marked Ghost");
				incorporealModes += '\0';
				incorporealModes += T(TKEY("incorporeal_either"), "Either");
				incorporealModes += '\0';
				ImGui::Combo(T(TKEY("incorporeal_mode"), "Ghosts Leave No Trench"), &settings.IncorporealMode, incorporealModes.c_str());
			}
			if (auto _ttIncorp = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("incorporeal_mode_tooltip"), "Stops things with no substance from digging the snow. This is a separate question from hovering, and has to be: a ghost stands with its feet on the ground like the Nord it otherwise is, so no clearance measurement will ever catch one. See-through bodies judges an actor by whether it is drawn solid, which needs no list and covers modded ghosts; actors marked Ghost reads the flag on the record instead, which never misses a ghost but also catches anything the game made unkillable rather than incorporeal."));

			ImGui::TreePop();
		}
		ImGui::PopID();

		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("spell_integration"), "Spell Integration"), ImGuiTreeNodeFlags_Framed)) {
		if (auto _ttSpell = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("spell_integration_tooltip"), "How magic marks the snow. Each school marks it differently: fire melts basins, and the others arrive with their own steps."));

		ImGui::Checkbox(T(TKEY("spell_enable"), "Enable Spell Integration"), &settings.EnableSpellIntegration);
		if (auto _ttSpellEnable = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("spell_enable_tooltip"), "Lets cast magic mark the snow. Off, the melt machinery still serves campfire clearings."));

		ImGui::Checkbox(T(TKEY("corpse_marks"), "Bodies Keep Marking"), &settings.CorpseElementalMarks);
		if (auto _ttCorpse = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("corpse_marks_tooltip"), "A body that is still alight, crackling or frozen over goes on working the snow it fell in - melting, pocking or glazing to match. Which one comes from whatever last struck the actor before it died, so nothing has to be read off the corpse itself. Mods that keep bodies burning or frozen long after death get the most out of this."));

		ImGui::SliderFloat(T(TKEY("corpse_seconds"), "Body Mark Duration"), &settings.CorpseEffectSeconds, 0.0f, 30.0f, "%.1f s");
		if (auto _ttCorpseSecs = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("corpse_seconds_tooltip"), "How long a body goes on marking after it falls. Vanilla death effects fade in a couple of seconds; mods that leave a corpse visibly burning or frozen run far longer, so match this to what you can actually see rather than to anything physical."));

		ImGui::SeparatorText(T(TKEY("spell_cat_force"), "Force (Shouts)"));
		if (auto _ttForce = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("spell_cat_force_tooltip"), "Force is the one school that moves snow without changing it. Everything else removes it, burns it or freezes it; a shove throws it forward and aside, deepest at the mouth and breaking up into streaks that thin out with distance, and piles what it moved along the rim. That is what makes Unrelenting Force read as pushed rather than deleted."));
		ImGui::Checkbox(T(TKEY("shout_cones"), "Shouts Plough The Snow"), &settings.EnableShoutCones);
		if (auto _ttShout = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("shout_cones_tooltip"), "Lets a shout work the whole oval of ground in front of the shouter rather than only the spot its projectile happens to strike. Each shout takes its reach from its own record, so a dragon's breath carries ten times a Greybeard's without anything being named. Off, shouts mark only through whatever they throw, and Whirlwind Sprint ploughs no furrow."));

		ImGui::SliderFloat(T(TKEY("shout_reach"), "Shout Mark Reach"), &settings.ShoutMarkReach, 0.1f, 2.0f, "%.2fx");
		if (auto _ttReach = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("shout_reach_tooltip"), "Multiplier on the reach each shout's own projectile authors - about 1000 units for Unrelenting Force, 1200 for the breaths, ten times that for a dragon. The far part of the mark fades and breaks up on its own, so this sets where the last streaks end, not where the solid scoop does. Aiming upward shortens it; aiming at the sky throws it away, since the snow layer cannot hold a mark in the air."));

		ImGui::SliderFloat(T(TKEY("shout_width"), "Shout Mark Width"), &settings.ShoutMarkWidth, 0.1f, 1.5f, "%.2f of reach");
		if (auto _ttWidth = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("shout_width_tooltip"), "How wide the mark is at its widest, as a fraction of its reach. The records author a shout's REACH but never its width, so this is the one thing about its shape that has to be taste rather than a reading."));

		ImGui::SliderFloat(T(TKEY("force_carve_depth"), "Force Carve Depth"), &settings.ForceCarveDepth, 0.0f, 1.0f, "%.2f");
		if (auto _ttForceDepth = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("force_carve_depth_tooltip"), "How deep a full-strength shove ploughs at the mouth, as a fraction of the layer; the mark gets shallower from there. Each shout scales down from here by the impact force its own record carries, so a dragon's shout bites harder than a breath's shove without either being named."));

		ImGui::SliderFloat(T(TKEY("shout_fade_start"), "Shout Mark Fade Start"), &settings.ShoutMarkFadeStart, 0.0f, 1.0f, "%.2f of reach");
		if (auto _ttFade = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("shout_fade_start_tooltip"), "How far along the mark the snow is thrown to full depth before it starts getting shallower. 0 fades from the mouth; 1 holds full depth to the end."));

		ImGui::SliderFloat(T(TKEY("shout_churn"), "Shout Churn"), &settings.ShoutChurn, 0.0f, 1.0f, "%.2f");
		if (auto _ttChurn = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("shout_churn_tooltip"), "How broken up a shout's mark is. 0 is a smooth scoop with a clean edge. At 1 a shove throws the snow near the mouth in uneven streaks, and further out only the strongest streaks land, so the mark thins into dashes before it ends. A breath's melt or frost stays solid; only its outline turns ragged. A shout that lays this mark leaves no other - the projectile it throws does not dig."));

		ImGui::PushID("spell_cat_force");
		if (ImGui::TreeNodeEx(T(TKEY("menu_advanced"), "Advanced"))) {
			ImGui::SliderFloat(T(TKEY("shout_waist"), "Shout Mark Widest Point"), &settings.ShoutMarkWaist, 0.1f, 0.9f, "%.2f of reach");
			if (auto _ttWaist = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("shout_waist_tooltip"), "Where along the reach the mark is widest. Low values swell it just ahead of the mouth and taper the rest; high values open it slowly and round off the far end."));

			ImGui::SliderFloat(T(TKEY("shout_roundness"), "Shout Mark Roundness"), &settings.ShoutMarkRoundness, 1.0f, 3.0f, "%.2f");
			if (auto _ttRound = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("shout_roundness_tooltip"), "The outline's curve. 1 is straight-sided - a kite from the mouth to the widest point and on to the end; 2 is an oval; higher squares the sides off."));

			ImGui::SliderFloat(T(TKEY("shout_far_depth"), "Shout Mark Far Depth"), &settings.ShoutMarkFarDepth, 0.0f, 1.0f, "%.2f");
			if (auto _ttFar = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("shout_far_depth_tooltip"), "Depth the mark fades to at its far end, against the depth at the mouth. What lands out there is the odd streak; this is how deep it goes."));

			ImGui::SliderFloat(T(TKEY("shout_streak_size"), "Shout Streak Size"), &settings.ShoutStreakSize, 8.0f, 80.0f, "%.0f");
			if (auto _ttStreak = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("shout_streak_size_tooltip"), "Width of the streaks the churn breaks a shove's mark into, in world units. Below a couple of map texels they blur together."));

			ImGui::SliderFloat(T(TKEY("shout_streak_length"), "Shout Streak Length"), &settings.ShoutStreakLength, 1.0f, 8.0f, "%.1fx");
			if (auto _ttStreakLen = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("shout_streak_length_tooltip"), "How much longer than wide each streak is, along the direction of the shout. 1 is blotches; higher draws them out into the lines snow thrown forward leaves."));

			ImGui::SliderFloat(T(TKEY("force_track"), "Travelling Shove Track"), &settings.ForceTrackWidth, 10.0f, 300.0f, "%.0f");
			if (auto _ttTrack = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("force_track_tooltip"), "Width of the trail left by a shove slow enough to watch travel - a cyclone rather than a shockwave. Those are told apart by speed alone: every shout blast crosses its own reach in about a second, while a cyclone crawls at barely above a sprint and takes four, so it leaves the line it took instead of a wedge. Nothing authors a width for it any more than for a cone, so this is taste."));

			ImGui::SliderFloat(T(TKEY("force_track_depth"), "Travelling Shove Depth"), &settings.ForceTrackDepth, 0.0f, 1.0f, "%.2f");
			if (auto _ttTrackDepth = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("force_track_depth_tooltip"), "How deep a travelling shove scours, against a full carve. A vortex scours the surface rather than digging to the ground - and the raised rim is derived from how deep the cut goes, so this is the dial that decides whether the trail reads as a scoured hollow or as a canyon with a ridge down each side."));

			ImGui::SliderFloat(T(TKEY("dash_gouge"), "Dash Furrow Width"), &settings.DashGougeScale, 0.1f, 3.0f, "%.2fx");
			if (auto _ttDash = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("dash_gouge_tooltip"), "Width of the furrow a shout ploughs when it throws its own caster forward, against the size of whatever is being thrown - so a dragon cuts a wider one than a man. Nothing is named here either: a self-delivered shout is simply watched for a moment, and what marks the snow is the caster moving faster than anything on foot can. A shout that leaves them standing marks nothing."));

			ImGui::TreePop();
		}
		ImGui::PopID();

		ImGui::SeparatorText(T(TKEY("spell_cat_fire"), "Fire"));
		if (auto _ttFire = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("spell_cat_fire_tooltip"), "Fire melts snow into soft basins. Unlike a footprint, a melt keeps deepening for as long as the heat stands over it."));
		ImGui::SliderFloat(T(TKEY("spell_melt_rate"), "Fire Melt Rate"), &settings.SpellMeltRate, 0.0f, 3.0f, "%.2f /s");
		if (auto _ttSpellRate = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("spell_melt_rate_tooltip"), "How fast a fire stream melts down to its basin, for a spell of Flames' strength; stronger spells scale up from here. This changes how quickly the basin appears, never how deep it ends up - depth is the bowl shape below."));

		ImGui::SliderFloat(T(TKEY("cloak_radius"), "Fire Cloak Radius"), &settings.CloakRadius, 60.0f, 400.0f, "%.0f");
		if (auto _ttCloak = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("cloak_radius_tooltip"), "How far a cloak marks the ground its wearer walks over. A cloak wraps the body rather than resting on the snow, so it digs more slowly than a flame played straight onto the ground - but it still reaches the floor if worn long enough."));

		ImGui::SliderFloat(T(TKEY("blast_radius_scale"), "Fire Blast Radius"), &settings.BlastRadiusScale, 0.1f, 2.0f, "%.2fx");
		if (auto _ttBlast = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("blast_radius_scale_tooltip"), "Size of the crater a detonation leaves, against the radius the explosion itself authors. Those radii are tuned for how far the blast HURTS, which is a good deal wider than the ground it should scar. Frost blasts and a frost atronach's death glaze are sized by it too."));

		ImGui::PushID("spell_cat_fire");
		if (ImGui::TreeNodeEx(T(TKEY("menu_advanced"), "Advanced"))) {
			ImGui::SliderFloat(T(TKEY("melt_persistence"), "Fire Melt Persistence"), &settings.MeltPersistence, 0.0f, 1.0f, "%.2f");
			if (auto _ttPersist = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("melt_persistence_tooltip"), "How much longer melted ground stays bare than trampled ground. The ground under a fire is warm and wet after the flame is gone, so a melt basin outlasts a footprint of the same depth. 0 = both recover at the same rate."));

			ImGui::SliderFloat(T(TKEY("melt_bowl_floor"), "Fire Melt Bowl Floor"), &settings.MeltBowlFloor, 0.0f, 0.9f, "%.2f");
			if (auto _ttBowl = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("melt_bowl_floor_tooltip"), "Shape of a melted hollow. 0 curves from the centre like a bowl, which is how heat actually spreads; higher values hold a flat floor and stand the sides up into walls, which reads as blasted rather than melted. Every frost glaze takes the same profile: higher holds the full glaze further out before it fades."));

			ImGui::SliderFloat(T(TKEY("melt_edge_irregularity"), "Fire Melt Edge Irregularity"), &settings.MeltEdgeIrregularity, 0.0f, 0.6f, "%.2f");
			if (auto _ttMeltEdge = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("melt_edge_irregularity_tooltip"), "How far a melted rim wanders off a perfect circle. This moves the outline only and leaves the surface smooth - a melt basin has a wandering edge but no jagged shards. The edge of every frost glaze wanders by the same amount."));

			ImGui::SliderFloat(T(TKEY("atronach_fire_reach"), "Fire Atronach Reach"), &settings.AtronachFireReach, 0.25f, 4.0f, "%.2fx");
			if (auto _ttAtroFire = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("atronach_fire_reach_tooltip"), "How much wider a fire atronach's own aura works than a cast fire cloak. Its whole body burns rather than a robe, and it hovers besides, so the circle it melts is broader and softer than anything a mage wears. Nothing about it is cast, so this is the only reach it has."));

			ImGui::SliderFloat(T(TKEY("atronach_fire_death"), "Fire Atronach Death Blast"), &settings.AtronachFireDeathRadius, 0.0f, 1200.0f, "%.0f");
			if (auto _ttAtroFireDeath = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("atronach_fire_death_tooltip"), "Radius of the burst a fire atronach leaves when it dies, before the Blast Radius setting above scales it. The default matches what the game authors for that explosion; it is a setting rather than a reading because the burst is spawned by script and never reaches this feature as anything we can measure."));

			ImGui::SliderFloat(T(TKEY("atronach_fire_burn"), "Fire Atronach Burn Time"), &settings.AtronachFireBurnSeconds, 0.0f, 30.0f, "%.1f s");
			if (auto _ttAtroBurn = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("atronach_fire_burn_tooltip"), "How long the body keeps melting the ground it fell on, after the burst. Unlike the burst this deepens for as long as it lasts, so it is what leaves the lasting scar where an atronach died. 0 leaves the burst alone."));

			ImGui::TreePop();
		}
		ImGui::PopID();

		ImGui::SeparatorText(T(TKEY("spell_cat_lightning"), "Lightning"));
		if (auto _ttShock = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("spell_cat_lightning_tooltip"), "Lightning does not melt snow, it throws it aside and burns what is left. A discharge pocks a small core with arc legs forking off it, and unlike a melt the displaced snow still piles into a rim."));
		ImGui::SliderFloat(T(TKEY("pit_depth"), "Lightning Pit Depth"), &settings.PitDepth, 0.0f, 1.0f, "%.2f");
		if (auto _ttPitDepth = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("pit_depth_tooltip"), "How deep a discharge pocks the snow, as a fraction of the layer: at 1 a full strike throws its deepest pocks clear to the ground, and the pocking leaves shallower snow between them. Unlike a melt it does not deepen with time, however long a cloak crackles over one spot."));

		ImGui::SliderFloat(T(TKEY("pit_radius"), "Lightning Pit Radius"), &settings.PitRadius, 20.0f, 200.0f, "%.0f");
		if (auto _ttPitRadius = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("pit_radius_tooltip"), "Reach of a single discharge before its arc legs, which fork out past it. Bigger spells scale up from here."));

		ImGui::SliderFloat(T(TKEY("scorch_strength"), "Lightning Scorch"), &settings.ScorchStrength, 0.0f, 1.0f, "%.2f");
		if (auto _ttScorch = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("scorch_strength_tooltip"), "How dark a discharge burns the snow it struck. 0 leaves the pocking alone and removes the blackening entirely."));

		ImGui::Checkbox(T(TKEY("lightning_arcs"), "Draw Lightning Arcs"), &settings.EnableLightningArcs);
		if (auto _ttArc = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("lightning_arcs_tooltip"), "Draws the bolt between a lightning cloak's wearer and the ground it just struck. Without it the snow pocks with nothing reaching the spot, which reads as a glitch rather than as lightning. Purely visual - nothing is spawned into the world, no damage, no sound."));

		ImGui::PushID("spell_cat_lightning");
		if (ImGui::TreeNodeEx(T(TKEY("menu_advanced"), "Advanced"))) {
			ImGui::SliderFloat(T(TKEY("arc_width"), "Arc Thickness"), &settings.LightningArcWidth, 1.0f, 40.0f, "%.0f units");
			if (auto _ttArcW = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("arc_width_tooltip"), "How thick the bolt is at its middle. It tapers to nothing at both ends whatever this is set to, so the hand and the ground never show a join."));

			ImGui::SliderFloat(T(TKEY("arc_brightness"), "Arc Brightness"), &settings.LightningArcBrightness, 0.0f, 20.0f, "%.1f");
			if (auto _ttArcB = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("arc_brightness_tooltip"), "Light added to the scene where the bolt is. It is additive, so this reads differently against night snow than against a sunlit slope."));

			ImGui::SliderFloat(T(TKEY("arc_life"), "Arc Duration"), &settings.LightningArcLife, 0.04f, 0.6f, "%.2f s");
			if (auto _ttArcL = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("arc_life_tooltip"), "How long one bolt lasts. It flares in over the first fraction and fades out across the rest, because a strike does not fade UP."));

			ImGui::ColorEdit3(T(TKEY("arc_tint"), "Arc Colour"), settings.LightningArcTint.data());
			if (auto _ttArcC = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("arc_tint_tooltip"), "Colour of the discharge."));

			ImGui::SliderFloat(T(TKEY("shock_cloak_radius"), "Lightning Cloak Radius"), &settings.ShockCloakRadius, 30.0f, 300.0f, "%.0f");
			if (auto _ttShockCloak = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("shock_cloak_radius_tooltip"), "How far a lightning cloak throws its arcs. Kept separate from the fire cloak's reach because the two behave differently: heat wraps the body, arcs jump clear of it."));

			ImGui::SliderFloat(T(TKEY("shock_cloak_interval"), "Lightning Cloak Interval"), &settings.ShockCloakInterval, 0.05f, 1.50f, "%.2f s");
			if (auto _ttShockRate = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("shock_cloak_interval_tooltip"), "Seconds between a lightning cloak's discharges. Higher is sparser - lightning cracks now and then rather than pouring out continuously."));

			ImGui::SliderFloat(T(TKEY("shock_cloak_strike_scale"), "Lightning Cloak Arc Size"), &settings.ShockCloakStrikeScale, 0.05f, 1.0f, "%.2f");
			if (auto _ttShockArc = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("shock_cloak_strike_scale_tooltip"), "Size of one arc against the cloak's own reach. Small values scatter fine pocks; large ones land marks nearly as wide as the cloak itself."));

			ImGui::SliderFloat(T(TKEY("atronach_shock_reach"), "Storm Atronach Reach"), &settings.AtronachShockReach, 0.25f, 4.0f, "%.2fx");
			if (auto _ttAtroShock = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("atronach_shock_reach_tooltip"), "How much further a storm atronach throws its arcs than a cast lightning cloak. Nothing about its aura is cast, so this is the only reach it has."));

			ImGui::SliderFloat(T(TKEY("atronach_shock_death"), "Storm Atronach Death Blast"), &settings.AtronachShockDeathRadius, 0.0f, 1200.0f, "%.0f");
			if (auto _ttAtroShockDeath = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("atronach_shock_death_tooltip"), "Radius a storm atronach discharges over when it dies, before the Blast Radius setting scales it. It earths itself and is finished, so unlike the fire one it leaves nothing burning afterwards."));

			ImGui::TreePop();
		}
		ImGui::PopID();

		ImGui::Checkbox(T(TKEY("decals_on_object_snow"), "Spell Marks Over Object Snow"), &settings.DecalsOnObjectSnow);
		if (auto _ttDecalsOver = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("decals_on_object_snow_tooltip"), "Scorch, frost and shock marks and rune glyphs are decals the game lays on a surface, and the thin snow coat on rocks and walls is drawn on top of them. With this on they are put back over the coat, as blood is. Bare rock is left alone."));

		ImGui::SeparatorText(T(TKEY("spell_cat_frost"), "Frost"));
		if (auto _ttFrost = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("spell_cat_frost_tooltip"), "Frost neither removes snow nor throws it aside - it refreezes what is there. Crusted snow bears weight, so tracks across it barely print; something heavy enough still breaks through. What frost looks like is the game's own frost mark, on every snow surface."));
		ImGui::Checkbox(T(TKEY("lift_frost"), "Raise Buried Frost Effects"), &settings.LiftFrostEffects);
		if (auto _ttLift = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("lift_frost_tooltip"), "Lifts a wall of frost, and the small ice effects like it, up onto the snow standing over them instead of leaving them buried underneath it. Frost is the only school this happens to: fire melts its own hole and lightning pits one, so those sit in snow they have already taken away, while frost only hardens what is there and leaves the full layer on top of itself. This is the one thing in the whole feature that MOVES something belonging to the game, so it has its own switch."));
		ImGui::Checkbox(T(TKEY("frost_decals"), "Frost Marks on Snow"), &settings.FrostDecalsOnSnow);
		if (auto _ttFrostDecals = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("frost_decals_tooltip"), "A frost spell leaves the game's own frost mark where it lands, and the landscape snow covers the ones on the ground beneath it. With this on, the mark is painted onto the snow over it, lit as the game lights the decal, and fades with it: on the landscape and road snow, and with Spell Marks Over Object Snow on object snow lying on what the mark covers, such as a walkway the snow pokes through."));

		ImGui::SliderFloat(T(TKEY("crust_rate"), "Frost Crust Rate"), &settings.CrustRate, 0.0f, 3.0f, "%.2f /s");
		if (auto _ttCrustRate = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("crust_rate_tooltip"), "How fast frost sets a crust, for a spell of Frostbite's strength. Like a melt this changes how quickly the glaze arrives, never how hard it ends up."));

		ImGui::SliderFloat(T(TKEY("crust_radius"), "Frost Crust Radius"), &settings.CrustRadius, 20.0f, 300.0f, "%.0f");
		if (auto _ttCrustRadius = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("crust_radius_tooltip"), "Reach of a frost source that states none of its own: a frost cloak, a frost atronach's aura (scaled by Frost Atronach Reach) and a body frozen where it fell (scaled by its size). A spell that authors its own area, like Blizzard, uses that instead and ignores this; a held stream keeps its own small reach."));

		ImGui::PushID("spell_cat_frost");
		if (ImGui::TreeNodeEx(T(TKEY("menu_advanced"), "Advanced"))) {
			ImGui::SliderFloat(T(TKEY("crust_print_depth"), "Frost Print Depth"), &settings.CrustPrintDepth, 0.0f, 1.0f, "%.2f");
			if (auto _ttCrustPrint = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("crust_print_depth_tooltip"), "How deep tracks still cut into fully crusted snow, against loose snow. Deliberately not zero: actors walk on the ground while the snow layer sits above them, so a crust nothing can mark buries feet inside what looks like solid ice."));

			ImGui::SliderFloat(T(TKEY("crust_break_carve"), "Frost Break On Carve"), &settings.CrustBreakOnCarve, 0.0f, 6.0f, "%.1f");
			if (auto _ttCrustCarve = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("crust_break_carve_tooltip"), "How completely cutting through a crust destroys it. Anything that cuts snow has broken the skin over it, so a trench should expose loose snow rather than staying polished all the way down - at 0 the glaze survives inside tracks and they read as grooves ploughed through ice cream."));

			ImGui::SliderFloat(T(TKEY("crust_thaw"), "Frost Thaw Time"), &settings.CrustThawMinutes, 0.0f, 30.0f, "%.1f min");
			if (auto _ttCrustThaw = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("crust_thaw_tooltip"), "How long a crust takes to melt away on its own. Ice answers to temperature rather than to weather, so this runs even under a clear sky, where snowfall has stopped and nothing else would ever remove it. 0 leaves a glaze standing until snow buries it."));

			ImGui::SliderFloat(T(TKEY("crust_break_radius"), "Frost Break Weight"), &settings.CrustBreakRadius, 8.0f, 120.0f, "%.0f");
			if (auto _ttCrustBreak = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("crust_break_radius_tooltip"), "Only creatures that carve by stamps read this: those beyond about 44 m from you or past the 16 nearest, such as a distant mammoth or a landing dragon. It is the size a body part must reach before it breaks through a crust instead of printing on it, standing in for weight: low values let anything shatter the glaze, high values let a mammoth walk on it. Nearer creatures carve by their own mesh and always print on a crust at Frost Print Depth."));

			ImGui::SliderFloat(T(TKEY("atronach_frost_reach"), "Frost Atronach Reach"), &settings.AtronachFrostReach, 0.25f, 4.0f, "%.2fx");
			if (auto _ttAtroFrost = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("atronach_frost_reach_tooltip"), "How much wider a frost atronach freezes than a cast frost source. Unlike the fire one it is heavy and walks, so it digs a trench AND glazes it over - the one thing in the whole feature that leaves a frozen groove, since fire removes snow and force only pushes it aside."));

			ImGui::SliderFloat(T(TKEY("atronach_frost_death"), "Frost Atronach Death Glaze"), &settings.AtronachFrostDeathRadius, 0.0f, 1200.0f, "%.0f");
			if (auto _ttAtroFrostDeath = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("atronach_frost_death_tooltip"), "Radius a frost atronach freezes over when it shatters, before the Blast Radius setting scales it. Deliberately far tighter than the fire one: it breaks apart where it stands rather than detonating outward."));

			ImGui::TreePop();
		}
		ImGui::PopID();

		ImGui::SeparatorText(T(TKEY("spell_cat_runes"), "Runes"));
		ImGui::Checkbox(T(TKEY("rune_decals"), "Rune Glyphs on Snow"), &settings.RuneDecalsOnSnow);
		if (auto _ttRuneDecal = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("rune_decals_tooltip"), "A rune's glyph is a decal the game lays on the ground, so deep snow covers it. With this on, the glyph is painted onto the snow over it instead, lit and glowing as the game lights the decal, and leaves with the rune: on the landscape and road snow, and with Spell Marks Over Object Snow on object snow lying on what the glyph covers."));
		ImGui::Checkbox(T(TKEY("lift_runes"), "Raise Buried Rune Effects"), &settings.LiftRunes);
		if (auto _ttLiftRunes = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("lift_runes_tooltip"), "Lifts a rune's shimmer, glow and cast flash onto the snow standing over them, so they play at the painted glyph rather than under it. Ground runes only - one cast on a wall stays where it is. What sets a rune off does not move: it still triggers from the ground."));


		ImGui::TreePop();
	}

#if !SNOW_ALPHA_BUILD
	if (ImGui::TreeNodeEx(T(TKEY("debug_options"), "Debugging Options"), ImGuiTreeNodeFlags_Framed)) {
		if (ImGui::TreeNodeEx(T(TKEY("debug_cat_contact"), "Actor & Prop Contact"))) {
			WrapTextF("  actors: %u living + %u corpses rasterized (caps %u / %u), %u partition draws + %u carried meshes (+%u sweep sub-steps; bone stamps skipped for these), %u overlays + %u fur shells declined, %u settled corpses latched out, %u hidden/orphan partitions declined, %u whole-skin-indexed, %u missing-bone stand-ins",
				stampStats.actorsRasterized, stampStats.corpsesRasterized, kContactMaxActors, kContactMaxCorpses, contactSkinDrawsLast, contactCarriedLast, contactSweepLast, contactOverlaysLast, contactShellsLast, stampStats.corpsesLatched, contactHiddenPartsLast, contactGlobalPartsLast, contactSkinMissingLast);

			CheckboxWrapped("Contact field view (what the rasterizer wrote this frame)", &debugContactView);

			if (debugContactView) {
				SliderFloatWrapped("Field view crop (units each side; 192 = a body, 1536 = the whole field)", &debugContactViewHalf, 64.0f, kContactHalfExtent, "%.0f");
				CheckboxWrapped("Object print view (Snow Trenches on Objects: each print texel's verdict left, the print map right)", &debugObjectPrintView);
				// The half-extent the view CS ran with, not the live slider: dragging
				// it rebuilds the image next frame, and drawing this frame's box at
				// next frame's scale is what makes a correct silhouette look misplaced.
				const float viewHalf = contactViewHalfUsed > 0.0f ? contactViewHalfUsed : std::clamp(debugContactViewHalf, 64.0f, kContactHalfExtent);
				if (debugObjectPrintView) {
					WrapTextF("Crop of %.0f x %.0f units around the player, %g units per view texel, +Y up. LEFT: each print texel's verdict this frame (black = no body over it, BLUE = the body is in the ground's snow layer, so the trench map takes it, MAGENTA = no captured object's top at the contact's height, green to yellow = a print, brighter = deeper). RIGHT: the print map over the SAME ground (red = slot A depth, green = slot B, the second surface level; faint blue = print texel edges when zoomed in; grey = outside the window). Yellow box = the player's world bound on both.",
						2.0f * viewHalf, 2.0f * viewHalf, 2.0f * viewHalf / (float)kContactViewPx);
					WrapTextF("  print map: %u texels a side (%.0f m reach), window corner (%.0f, %.0f) | %zu live tiles, %zu stored | last frame: %u stamp blocks, %u refill blocks, %u tiles mirrored, %u injected, %u drapes on the print twin%s%s",
						printMapDim, 0.5f * (float)printMapDim * kPrintTexel / kUnitsPerMeter,
						(float)printWindowMin.x * kPrintTexel, (float)printWindowMin.y * kPrintTexel,
						printLive.size(), printStoreTilesLast, printStampBlocksLast, printRefillBlocksLast, printMirrorsLast, printInjectsLast,
						printTwinDrawsLast, staticsPSDrapePrintsNoDepth ? "" : " (TWIN NOT COMPILED)", printCSFailed ? " | KERNELS FAILED TO COMPILE" : "");
				} else {
					WrapTextF("Crop of %.0f x %.0f units around the player, %g units per view texel, +Y up. LEFT: the contact field as the carve pass samples it (red = carve fraction, green = hovering, MAGENTA = the surface reads below that column's ground, which carves full depth without touching anything, black = nothing drawn). RIGHT: the deformation map over the SAME ground (red = carve depth, faint blue = map texel edges). Yellow box = the player's world bound on both.",
						2.0f * viewHalf, 2.0f * viewHalf, 2.0f * viewHalf / (float)kContactViewPx);
				}
				if (contactViewSRV) {
					// Two squares side by side, sized to the window.
					const float box = std::max(128.0f, std::floor((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f));
					const float scale = box / (2.0f * viewHalf);
					const float2 center = contactViewCenter;
					auto drawHalf = [&](float a_v0, float a_v1) {
						const ImVec2 topLeft = ImGui::GetCursorScreenPos();
						ImGui::Image(contactViewSRV.get(), { box, box }, { 0.0f, a_v0 }, { 1.0f, a_v1 });
						if (auto* player = globals::game::player) {
							if (auto* root = player->Get3D(false)) {
								auto toX = [&](float a_wx) { return topLeft.x + (a_wx - (center.x - viewHalf)) * scale; };
								auto toY = [&](float a_wy) { return topLeft.y + ((center.y + viewHalf) - a_wy) * scale; };
								// The bound the crop was BUILT from, so the box is a
								// statement about the image rather than about the
								// moment the menu happened to read the scene graph.
								const float2 boundCenter = contactViewBoundCenter;
								const float boundRadius = contactViewBoundRadius;
								if (boundRadius > 0.0f)
									ImGui::GetWindowDrawList()->AddRect(
										{ toX(boundCenter.x - boundRadius), toY(boundCenter.y + boundRadius) },
										{ toX(boundCenter.x + boundRadius), toY(boundCenter.y - boundRadius) },
										IM_COL32(255, 220, 80, 220), 0.0f, 0, 1.5f);
								// Bone positions over the silhouette: feet white, hands cyan,
								// head/spine magenta. A mesh stretched against its own bones
								// shows as a silhouette reaching past them.
								struct BoneDot { const char* name; ImU32 color; };
								static const BoneDot dots[] = {
									{ "NPC L Foot [Lft ]", IM_COL32(255, 255, 255, 255) }, { "NPC R Foot [Rft ]", IM_COL32(255, 255, 255, 255) },
									{ "NPC L Hand [LHnd]", IM_COL32(80, 240, 255, 255) }, { "NPC R Hand [RHnd]", IM_COL32(80, 240, 255, 255) },
									{ "NPC Head [Head]", IM_COL32(255, 80, 255, 255) }, { "NPC Spine2 [Spn2]", IM_COL32(255, 80, 255, 255) },
									{ "NPC L Calf [LClf]", IM_COL32(200, 200, 200, 255) }, { "NPC R Calf [RClf]", IM_COL32(200, 200, 200, 255) },
								};
								for (const auto& dot : dots) {
									if (auto* node = root->GetObjectByName(RE::BSFixedString(dot.name)))
										ImGui::GetWindowDrawList()->AddCircle({ toX(node->world.translate.x), toY(node->world.translate.y) }, 4.0f, dot.color, 0, 1.5f);
								}
							}
						}
					};
					drawHalf(0.0f, 0.5f);
					ImGui::SameLine();
					drawHalf(0.5f, 1.0f);
					if (auto* player = globals::game::player) {
						if (auto* root = player->Get3D(false))
							// Crop centre beside the live bound: the box is drawn from the
							// crop, so a gap between these two is the only way the box can
							// sit off a centred silhouette. Named here so one screenshot
							// separates a misplaced overlay from a misplaced raster.
							WrapTextF("player bound: centre (%.0f, %.0f) radius %.0f | yaw %.2f rad | crop centre (%.0f, %.0f) half %.0f, live bound drifts (%.0f, %.0f) | field centre (%.0f, %.0f) | map texel %.2f units, contact texel %g units | dots: feet white, calves grey, hands cyan, head/spine magenta",
								root->worldBound.center.x, root->worldBound.center.y, root->worldBound.radius, player->GetAngleZ(),
								contactViewCenter.x, contactViewCenter.y, contactViewHalfUsed,
								root->worldBound.center.x - contactViewCenter.x, root->worldBound.center.y - contactViewCenter.y,
								contactCenter.x, contactCenter.y,
								contactViewTexelSize, 2.0f * kContactHalfExtent / (float)kContactDim);
					}
					// Heights against the baked ground: what the carve's (contact - ground) / layer
					// sees, in numbers. Red arms with hands 60 units up means the raster's Z
					// or the ground sample is wrong, not the silhouette.
					if (auto* player = globals::game::player) {
						if (auto* root = player->Get3D(false)) {
							const auto probe = ProbeShellData(player->GetPositionX(), player->GetPositionY());
							WrapTextF("  ground at player (baked vertex): %.1f, layer %.1f | player Z %.1f (%.1f above ground)",
								probe.height, probe.rampDepth, player->GetPositionZ(), player->GetPositionZ() - probe.height);
							static const char* heightBones[] = { "NPC L Foot [Lft ]", "NPC R Foot [Rft ]", "NPC L Hand [LHnd]", "NPC R Hand [RHnd]", "NPC Head [Head]" };
							std::string line = "  bone Z above ground:";
							for (const char* name : heightBones) {
								if (auto* node = root->GetObjectByName(RE::BSFixedString(name)))
									line += std::format(" {} {:.0f} |", name, node->world.translate.z - probe.height);
							}
							WrapText(line.c_str());
						}
					}
				} else {
					WrapTextF("(no field this frame)");
				}
			}

			WrapTextF("  contact: %u props rasterized, %u draws, window %.0f m", stampStats.propsRasterized, contactDrawsLast, kContactHalfExtent / kUnitsPerMeter);

			ImGui::Checkbox("Skeleton Probe (nearest NPC)", &debugSkeletonProbe);
			if (debugSkeletonProbe) {
				ImGui::SameLine();
				if (ImGui::Button("Dump skeleton to log"))
					skeletonProbeDumpRequested = true;
				if (!skeletonProbe.valid) {
					WrapTextF("probe: no NPC in range this frame");
				} else {
					WrapTextF("probe: %s (%08X) - %s",
						skeletonProbe.actorName.empty() ? "<unnamed>" : skeletonProbe.actorName.c_str(),
						skeletonProbe.formID, skeletonProbe.verdict);
					if (skeletonProbe.rasterCandidate)
						WrapTextF("  contact pass candidate: airborne/elevated/floating gates bypassed, penetration decides");
					WrapTextF("  feet %zu (usable %u) | limbs %u (stamped %u) | shapes stamped %u | dry travel %.0f%s",
						skeletonProbe.feet.size(), skeletonProbe.usableFeet,
						skeletonProbe.limbs, skeletonProbe.limbsStamped, skeletonProbe.shapes,
						skeletonProbe.dryTravel, skeletonProbe.collisionFallback ? " [FAILSAFE]" : "");
					WrapTextF("  body alpha %.2f on '%s' (reads %u) | above land %.0f | floating gap %.0f",
						skeletonProbe.bodyAlpha, skeletonProbe.bodyAlphaSource.c_str(), (uint)skeletonProbe.alphaSettle,
						skeletonProbe.gapToLand, skeletonProbe.floatingGap);
					{
						const char* surface;
						if (!skeletonProbe.cellBaked)
							surface = "cell not baked yet (stamps assume snow)";
						else if (skeletonProbe.shellDepth <= 0.0f)
							surface = "NO SHELL HERE: ground class carries no snow - nothing can display a trench";
						else if (skeletonProbe.gapToLand > 10.0f)
							surface = "standing on a mesh ABOVE the landscape - snow there is object snow (trenches on roads, prints elsewhere from Snow Trenches on Objects)";
						else
							surface = "landscape shell underfoot - trenches should show";
						WrapTextF("  ground: shell depth %.0f | %s", skeletonProbe.shellDepth, surface);
					}
					if (!skeletonProbe.feet.empty() &&
						ImGui::BeginTable("##skelprobe", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
						ImGui::TableSetupColumn("Foot node");
						ImGui::TableSetupColumn("Toe");
						ImGui::TableSetupColumn("Scale");
						ImGui::TableSetupColumn("Attached");
						ImGui::TableSetupColumn("z-ref / band");
						ImGui::TableSetupColumn("Result");
						ImGui::TableHeadersRow();
						for (const auto& row : skeletonProbe.feet) {
							ImGui::TableNextRow();
							ImGui::TableNextColumn();
							ImGui::TextUnformatted(row.name.c_str());
							ImGui::TableNextColumn();
							ImGui::TextUnformatted(row.toe.c_str());
							ImGui::TableNextColumn();
							ImGui::Text("%.3f", row.scale);
							ImGui::TableNextColumn();
							ImGui::TextUnformatted(row.attached ? "yes" : "DETACHED");
							ImGui::TableNextColumn();
							ImGui::Text("%.1f / %.1f", row.zAboveRef, row.band);
							ImGui::TableNextColumn();
							if (row.stamped)
								ImGui::Text("STAMPED r=%.1f", row.radius);
							else if (row.planted)
								ImGui::TextUnformatted("planted, no stamp");
							else if (row.scale < 0.01f)
								ImGui::TextUnformatted("ZERO SCALE");
							else
								ImGui::TextUnformatted("lifted");
						}
						ImGui::EndTable();
					}
				}
			}

			SliderIntWrapped("Solo skinned geometry (-1 = all; step through to find whose silhouette is wide)", &debugContactSolo, -1, 31);

			if (debugContactSolo >= 0)
				WrapTextF("  soloed: %s", contactSoloName.c_str());

			// Diagnostics: plain text by existing convention (no i18n).
			WrapTextF("Stamps/frame: feet %u, limbs %u, shapes %u, props %u (refs at last scan %u, movers %u, scan every 6 frames)",
				stampStats.feet, stampStats.limbs, stampStats.shapes, stampStats.props,
				propScanRefs, stampStats.propMovers);
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx(T(TKEY("debug_cat_blood"), "Blood Decals"))) {
			WrapTextDisabledF("%s", std::format("{} decals tracked, {} marks + {} discs deposited, {} drawn over object snow this frame", bloodSeenLive, bloodDepositsLast, bloodDiscsLast, bloodOverlaysLast).c_str());
			WrapTextDisabledF("%s", std::format("Detail tiles: {} of {} live, {} merged this frame; mark bounds {} from vertices, {} guessed", bloodTilesLive, BloodTileCount(), bloodTileMergesLast, bloodTileBoundsRead, bloodTileBoundsGuessed).c_str());
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx(T(TKEY("debug_cat_cpu"), "CPU & Memory"))) {
			// CPU census (PERF-RESEARCH §11.1), 30-frame averages. The hook line
			// is the one cost no profiler row carries.
			{
				cpuCensusArmedUntil = globals::state->frameCount + 120;
				const auto& s = cpuShown;
				WrapTextF("CPU: capture hook %.3f ms over %.0f draws | gather: %.0f actors %.3f ms, land %.0f calls %.3f ms, depth %.0f calls %.3f ms, collision walks %.0f in %.3f ms",
					s.hookMs, s.hookCalls, s.actors, s.actorMs, s.landCalls, s.landMs, s.depthCalls, s.depthMs, s.traverseCalls, s.traverseMs);
				WrapTextF("Submission: skin loop %.0f draws, %.0f CB updates", s.skinLoopDraws, s.skinLoopCBUpdates);
				WrapTextF("Capture hash %016llX over %u skins", (unsigned long long)cpuCensus.captureHash, cpuCensus.captureCount);
				WrapTextF("Stamp hash %016llX, unchanged for %u frames", (unsigned long long)cpuCensus.stampHash, cpuCensus.stampHashStable);
				ImGui::SameLine();
				if (ImGui::Button("Dump Stamp Hash Ring"))
					cpuCensus.stampHashDumpRequested = true;
				if (auto _ttRing = Util::HoverTooltipWrapper())
					ImGui::Text("%s", T(TKEY("stamp_hash_ring_tooltip"), "Writes the last 300 frames' stamp checksums to the log. Record one with a lever off and one with it on, in the same scene with the same motion; matching sequences mean the change did not touch what reaches the deformation map."));
			}

			uint64_t vramUsageMB = 0, vramBudgetMB = 0;
			QueryAdapterVRAM(vramUsageMB, vramBudgetMB);
			std::string vramBreakdown;
			const uint64_t vramFeatureMB = SumFeatureTextureBytes(vramBreakdown) >> 20;
			WrapTextF("VRAM: adapter %llu / %llu MB (%llu%%), this feature ~%llu MB",
				(unsigned long long)vramUsageMB, (unsigned long long)vramBudgetMB,
				(unsigned long long)(vramBudgetMB ? vramUsageMB * 100 / vramBudgetMB : 0),
				(unsigned long long)vramFeatureMB);
			if (vramBudgetMB && vramUsageMB > vramBudgetMB)
				WrapTextColoredF(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "OVER BUDGET: driver is demoting textures to system RAM; FPS stays degraded until the game restarts.");
			WrapText(vramBreakdown.c_str());

			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx(T(TKEY("debug_cat_deform_map"), "Deformation Map"))) {
			if (ImGui::Button(T(TKEY("clear"), "Clear Map"))) {
				clearRequested = true;
				// Deliberate wipe, so the store goes with it: left alone, the
				// inject would put every trench back on the very next frame.
				ClearTrenchStore("the Clear Deformation Map button");
				ClearPrintStore("the Clear Deformation Map button");
			}


			// S3: the tile dispatch's one-click cross-check. A symptom that
			// vanishes with this on means a dirty-tracking path missed a writer.
			ImGui::Checkbox(T(TKEY("debug_force_tiles"), "Force All Tiles Dirty"), &debugForceAllTilesDirty);

			// S1: the idle skip and its measurement override. The readout names
			// what is holding the pass on, so "why is it running" answers itself.
			ImGui::Checkbox(T(TKEY("debug_force_update"), "Force Update"), &debugForceDeformationUpdate);

			ImGui::Checkbox(T(TKEY("show_debug"), "Show Map"), &showDebugTexture);
			if (showDebugTexture) {
				// The map's own depth (TrenchDebugCS), copied to a single channel:
				// ImGui blends by the texture's alpha, and the map's .w is the impact
				// energy, so the map drawn directly shows only where something hit
				// the snow. This is the image that answers "did the trench reach the map".
				WrapTextF("%s", T(TKEY("debug_hint"), "Deformation map: red = compressed snow. The window is centred on the camera, so the cross is you and the ring is 25 m."));
				const ImVec2 imageTopLeft = ImGui::GetCursorScreenPos();
				if (trenchDebugSRV)
					ImGui::Image(trenchDebugSRV.get(), { 512.0f, 512.0f });
				else if (auto* deformSRV = GetDeformationSRV())
					ImGui::Image(deformSRV, { 512.0f, 512.0f });

				// Where the player is, drawn ON the map. Without it "the trench did
				// not load" and "the trench is eighty metres that way" look
				// identical: the window is 14000 units across, so a mark 200 px off
				// centre is most of a hundred metres away.
				{
					auto* draw = ImGui::GetWindowDrawList();
					const ImVec2 centre{ imageTopLeft.x + 256.0f, imageTopLeft.y + 256.0f };
					const float pixelsPerUnit = 512.0f / std::max(deformWorldSize, 1.0f);
					const float ring = 25.0f * kUnitsPerMeter * pixelsPerUnit;
					const ImU32 ink = IM_COL32(80, 220, 255, 220);
					draw->AddLine({ centre.x - 8.0f, centre.y }, { centre.x + 8.0f, centre.y }, ink, 1.5f);
					draw->AddLine({ centre.x, centre.y - 8.0f }, { centre.x, centre.y + 8.0f }, ink, 1.5f);
					draw->AddCircle(centre, ring, IM_COL32(80, 220, 255, 90), 0, 1.0f);
				}

				// R8_UNORM samples as (depth, 0, 0, 1), so this one is opaque and
				// can be trusted. It is the store's own answer to "what does the
				// window look like", which is exactly the question a reload raises.
				if (trenchInjectSRV) {
					WrapTextF("%s", T(TKEY("debug_inject_hint"), "Trench store: what the inject last painted into the window. Opaque, so what you see is what the store holds. Refreshed when the window scrolls or is rebuilt - after a load it is the whole restored window."));
					ImGui::Image(trenchInjectSRV.get(), { 512.0f, 512.0f });
				}
			}


			// The count is the magnitude behind a map-active blocker: a handful of
			// texels is a precision tail, millions is a logic bug. From the newest
			// verdict, so it lags the dispatch by the readback ring.
			ImGui::Checkbox(T(TKEY("debug_activity_view"), "Update Activity View"), &debugActivityView);
			if (debugActivityView) {
				WrapTextF("Texels the last pass changed at stored precision. R = depth, G = melt/scorch, B = crust or impact energy. Black = the pass rewrote the map byte-identically. The yellow box bounds the changed texels - a few hundred are sub-pixel here without it.");
				const ImVec2 activityTopLeft = ImGui::GetCursorScreenPos();
				if (activityViewSRV)
					ImGui::Image(activityViewSRV.get(), { 512.0f, 512.0f });

				const bool bboxValid = deformChangedTexels > 0 &&
				                       deformChangedMinX <= deformChangedMaxX &&
				                       deformChangedMinY <= deformChangedMaxY;
				if (bboxValid && activityViewSRV) {
					const float scale = 512.0f / std::max((float)deformMapDim, 1.0f);
					auto* draw = ImGui::GetWindowDrawList();
					draw->AddRect(
						{ activityTopLeft.x + (float)deformChangedMinX * scale - 2.0f,
							activityTopLeft.y + (float)deformChangedMinY * scale - 2.0f },
						{ activityTopLeft.x + (float)(deformChangedMaxX + 1) * scale + 2.0f,
							activityTopLeft.y + (float)(deformChangedMaxY + 1) * scale + 2.0f },
						IM_COL32(255, 220, 80, 220), 0.0f, 0, 1.5f);
				}

				// The mean delta is the fingerprint: the slump step is
				// SlumpRate x 0.5 x dt (~0.0008 at defaults) and scales with the
				// Snow Slumping slider; storage-precision creep is an order
				// smaller and scales with nothing.
				WrapTextF("Changed texels (last verdict): %u (depth %u, melt/scorch %u, crust/impact %u), mean delta %.5f",
					deformChangedTexels, deformChangedDepth, deformChangedMelt, deformChangedCrustDep,
					deformChangedTexels > 0 ? (double)deformChangedDeltaSum * 1e-6 / (double)deformChangedTexels : 0.0);
				if (bboxValid) {
					const float texel = deformWorldSize / std::max((float)deformMapDim, 1.0f);
					const float cx = ((float)(deformChangedMinX + deformChangedMaxX) * 0.5f + 0.5f) * texel;
					const float cy = ((float)(deformChangedMinY + deformChangedMaxY) * 0.5f + 0.5f) * texel;
					const float half = deformWorldSize * 0.5f;
					WrapTextF("Bbox: (%u,%u)-(%u,%u), %.1f x %.1f m, centre %.1f m E / %.1f m N of camera",
						deformChangedMinX, deformChangedMinY, deformChangedMaxX, deformChangedMaxY,
						(float)(deformChangedMaxX - deformChangedMinX + 1) * texel / kUnitsPerMeter,
						(float)(deformChangedMaxY - deformChangedMinY + 1) * texel / kUnitsPerMeter,
						(cx - half) / kUnitsPerMeter, (cy - half) / kUnitsPerMeter);
				}
			}


			if (deformIdleSkipped) {
				if (deformSkipRate >= 0.0f)
					WrapTextF("Update pass: idle (skipped) - %.0f%% of last 300 frames", deformSkipRate * 100.0f);
				else
					WrapTextF("Update pass: idle (skipped)");
			} else {
				static const char* kBlockerNames[9] = { "scroll", "stamps", "inject", "refill", "clear", "map-active", "verdict-stale", "contact", "object spells" };
				std::string held;
				for (int bit = 0; bit < 9; bit++)
					if (deformIdleBlockers & (1u << bit)) {
						if (!held.empty())
							held += ", ";
						held += kBlockerNames[bit];
					}
				if (debugForceDeformationUpdate)
					held = held.empty() ? "forced" : "forced, " + held;
				if (deformSkipRate >= 0.0f)
					WrapTextF("Update pass: running (%s) - skipped %.0f%% of last 300 frames",
						held.empty() ? "none - engages next verdict" : held.c_str(), deformSkipRate * 100.0f);
				else
					WrapTextF("Update pass: running (%s)", held.empty() ? "none - engages next verdict" : held.c_str());
				// Within a running frame the evolve pass has its own gate: idle
				// means its last full run changed nothing and no external write
				// (ring inject, stamps) or refill has re-armed it - the ring and
				// stamp passes are the only cost while walking settled ground.
				WrapTextF("Evolve pass: %s", evolveIdleLastFrame ? "idle (settled)" : "active");
			}

			if (deformSkipRate >= 0.0f) {
				// The flicker census: count changes are stamps appearing or
				// vanishing (plant-band flicker), drift is a matched stamp moving
				// past tolerance. Either resets the quiet window.
				WrapTextF("Stamp set changes: %u/300 frames (count %u, drift %u)",
					stampSetCountChanges + stampSetDriftChanges, stampSetCountChanges, stampSetDriftChanges);
			}

			{
				// The tile census: dispatch domain vs the whole map. Cost should
				// track these counts; if it does not, the force-all-dirty toggle
				// is the discriminator.
				const uint32_t totalTiles = (deformMapDim / 8) * (deformMapDim / 8);
				if (debugForceAllTilesDirty)
					WrapTextF("Stamp tiles: forced to full map (%u tiles)", totalTiles);
				else if (stampTilesLast > kStampTileCap)
					WrapTextF("Stamp tiles: OVERFLOWED to full map (%u tiles)", totalTiles);
				else
					WrapTextF("Stamp tiles: %u of %u", stampTilesLast, totalTiles);
				// Occupied + slump halo and changed + tap halo, from the last
				// executed scans (lag by the readback ring, like the verdict).
				WrapTextF("Evolve tiles: %u of %u", evolveTilesLast, totalTiles);
				WrapTextF("Berm tiles: %u of %u", bermTilesLast, totalTiles);
			}


			{
				// One tile is 512 world units square, only trodden ground has one,
				// and refill deletes the ones it takes back to bare snow. Occupancy
				// separates real trails from tiles a shallow refill residue is
				// keeping alive: a high thin count wants a bigger store epsilon,
				// not a faster fade.
				const auto stats = GetTrenchStoreStats();
				WrapTextF("Trench store: %zu tiles, %.1f KB raw, %.1f%% full, %zu thin",
					stats.tiles, (double)stats.bytes / 1024.0, stats.occupancy * 100.0f, stats.thin);
				// Encoded is the number that matters: it is what a save will cost
				// once Stage C writes it, and what the budget is spent in.
				WrapTextF("Save cost: %.0f KB of %.0f KB budget (%.0fx vs raw)",
					(double)stats.encoded / 1024.0,
					(double)settings.TrenchMemoryMB * 1024.0,
					stats.encoded ? (double)stats.bytes / (double)stats.encoded : 0.0);
			}

			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx(T(TKEY("debug_cat_shell"), "Landscape Shell"))) {
			if (effectCost.running) {
				ImGui::BeginDisabled();
				ImGui::Button(std::format("Measuring... {} of {}", effectCost.step / 2 + 1, EffectCostProbe::kEffects).c_str());
				ImGui::EndDisabled();
			} else if (ImGui::Button(T(TKEY("effect_cost_probe"), "Measure Effect Costs"))) {
				StartEffectCostProbe();
			}
			if (auto _ttEffectCost = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("effect_cost_probe_tooltip"), "Turns each snow shading effect off in turn for about two seconds and shows how much GPU time the snow saves without it. Covers the ground snow and the snow on objects, which share these effects. Stand still and keep the camera still while it runs (about 50 seconds); the effects visibly switch off one by one. Differences under about 0.03 ms are measurement noise."));
			if (effectCost.done) {
				// Diagnostics: plain text by existing convention (no i18n).
				const auto [lo, hi] = std::minmax_element(std::begin(effectCost.withMs), std::end(effectCost.withMs));
				WrapTextF("Snow shading + depth prepasses with everything on: %.3f ms (drift during the run %.3f ms)", effectCost.withMs[0], *hi - *lo);
				for (uint32_t i = 0; i < EffectCostProbe::kEffects; i++)
					WrapTextF("  %-28s %6.3f ms", EffectCostProbe::kNames[i], effectCost.withMs[i] - effectCost.withoutMs[i]);
			}
			ImGui::Checkbox(T(TKEY("land_recolor"), "Recolor Land Snow (A/B)"), &landRecolor);
			if (auto _ttLandRecolor = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("land_recolor_tooltip"), "Where the land itself shows (prints down to the ground, thin snow, the bases of blended objects), its snow textures wear this mod's snow, texel by texel. Rocks, grass and dirt inside those textures keep their own look."));
			WrapTextF("  %s", LandscapeSnowStatus().c_str());

			ImGui::Checkbox(T(TKEY("shell_pipeline_stats"), "Pipeline Statistics"), &shellPipelineStatsEnabled);
			if (auto _ttStats = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("shell_pipeline_stats_tooltip"), "Hardware pipeline-statistics and occlusion queries around the ground snow's draws (when its prepass runs late, in DrawShell) and the object-snow pass, read back two frames later without stalling. PS invocations divided by samples passed is how many pixel-shader runs each visible pixel costs - overdraw times quad overshade. The DS/HS/VS counts check the geometry-stage arithmetic. A line counts all of that pass's draws - the prepass's own cheap invocations and the fullscreen passes' samples are in there - so read the profiler rows as the verdict and these lines as the explanation."));

			if (shellPipelineStatsEnabled) {
				auto statsLine = [](const char* a_label, const ShellStatsResult& a_r) {
					if (!a_r.valid) {
						WrapTextF("%s: waiting for query results", a_label);
						return;
					}
					const double perVisible = a_r.samplesPassed ? static_cast<double>(a_r.psInvocations) / static_cast<double>(a_r.samplesPassed) : 0.0;
					WrapTextF("%s: PS %.2f M inv / %.2f M px visible = %.2f per px; prims %.2f M; VS %.2f M, HS %.2f M, DS %.2f M",
						a_label,
						a_r.psInvocations / 1e6, a_r.samplesPassed / 1e6, perVisible,
						a_r.rasterizedPrimitives / 1e6,
						a_r.vsInvocations / 1e6, a_r.hsInvocations / 1e6, a_r.dsInvocations / 1e6);
				};
				statsLine("Ground snow", shellStatsLast);
				statsLine("Object snow", staticsStatsLast);
				statsLine("Trench patch (in object snow)", patchStatsLast);
			}


			ImGui::Checkbox(T(TKEY("debug_overlay"), "Terrain Overlay"), &debugTerrainOverlay);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("debug_overlay_tooltip"), "Paints diagnostics on terrain: red = outside deformation window, green = deformation, blue = detected snow."));

			WrapTextF("Exclusion zones: %u, workspace clearings: %u, sealed containers: %u (Survival heat list %s)",
				statExclusionCount, statTrampleCount, statSealedCount, survivalHeatSources ? "found" : "absent");

			WrapTextF("Shadow source: descriptors=%u endSplits=%.0f/%.0f/%.0f atlasSlices=%u",
				dbgLodDescriptorCount, dbgLodEndSplits[0], dbgLodEndSplits[1], dbgLodEndSplits[2], dbgLodAtlasSlices);


			// Shell probe: what the landscape shell reads at the camera. A shell
			// artifact that this call cannot account for is not the landscape
			// shell - it is object snow, which the Object Snow debug view owns.
			{
				auto eye = globals::game::frameBufferCached.GetCameraPosAdjust();
				const ShellProbe probe = ProbeShellData(eye.x, eye.y);
				const char* worldspaceName = "none";
				if (auto* tes = globals::game::tes)
					if (auto* worldspace = tes->GetRuntimeData2().worldSpace)
						worldspaceName = worldspace->GetFormEditorID();

				WrapTextF("Shell probe @ (%.0f, %.0f) cell (%d, %d) vertex (%d, %d)",
					probe.worldX, probe.worldY, probe.cellX, probe.cellY, probe.vertexX, probe.vertexY);
				WrapTextF("  worldspace %08X %s, window built for %08X",
					probe.activeWorldspaceID, worldspaceName, probe.windowWorldspaceID);
				if (!probe.cellFound) {
					WrapTextF("  no baked cell: the shell has NO terrain data here");
				} else if (!probe.worldspaceMatch) {
					WrapTextF("  cell baked in worldspace %08X: REJECTED, shell has no data here", probe.cellWorldspace);
				} else {
					WrapTextF("  height %.0f, depth %.1f, coverage %.2f -> surface %.0f",
						probe.height, probe.rampDepth, probe.coverage, probe.height + probe.rampDepth);
					for (const auto& layer : probe.layers)
						WrapTextF("    %s x%.2f @ %.0f units", layer.label.c_str(), layer.weight, layer.depth);
				}

				// Object snow probe: which captured meshes cover this spot, largest
				// first. Bound top Z against the camera height says whether one of
				// them is the surface an artifact sits on.
				const ObjectSnowProbe objects = ProbeObjectSnow(eye.x, eye.y);
				WrapTextF("Object snow probe: %zu captured this frame, %zu cover this spot (camera z %.0f)",
					objects.captured, objects.overlapping, eye.z);
				for (const auto& entry : objects.entries)
					WrapTextF("  %s r%.0f top %.0f (%.0f away)%s  %s",
						entry.name.empty() ? "<unnamed>" : entry.name.c_str(),
						entry.radius, entry.topZ, entry.distXY, entry.road ? " [road]" : "", entry.model.c_str());
			}

			WrapTextF("Snow mask cache: %zu entries, %llu hits, %llu misses",
				snowMasksSizeForUI(),
				(unsigned long long)landMaskHits.load(std::memory_order_relaxed),
				(unsigned long long)landMaskMisses.load(std::memory_order_relaxed));

			WrapTextF("Terrain data: %zu cells baked, %u in window, %u snow texels, height range [%.0f, %.0f]",
				ShellCellCountForUI(), shellStatCellsInWindow, shellStatSnowTexels,
				shellStatMinHeight, shellStatMaxHeight);
			WrapTextF("Land-exact layer: %u of %d cells from the land mesh, the rest cubic", shellFineMeshCells, kShellFineCells * kShellFineCells);

			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx(T(TKEY("debug_cat_melt_emitter"), "Melt Emitter"))) {
			if (auto _ttEmitterCat = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("debug_cat_melt_emitter_tooltip"), "A stand-in heat source that answers to no spell at all. Kept for testing a mark on its own: when a school stops marking, this says whether the fault is in the detector or in the mark itself."));
			if (ImGui::Button(T(TKEY("melt_emitter_drop"), "Drop Melt Emitter Here"))) {
				if (auto* player = globals::game::player) {
					debugMeltEmitterPos = player->GetPosition();
					debugMeltEmitterActive = true;
				}
			}
			ImGui::SameLine();
			if (ImGui::Button(T(TKEY("melt_emitter_remove"), "Remove")))
				debugMeltEmitterActive = false;
			WrapTextF("%s", debugMeltEmitterActive ?
								  T(TKEY("melt_emitter_active"), "Emitter: active") :
								  T(TKEY("melt_emitter_off"), "Emitter: off"));


			ImGui::SliderFloat(T(TKEY("melt_emitter_radius"), "Emitter Radius"), &debugMeltEmitterRadius, 40.0f, 600.0f, "%.0f");


			ImGui::SliderFloat(T(TKEY("melt_emitter_rate"), "Emitter Melt Rate"), &debugMeltEmitterRate, 0.02f, 2.0f, "%.2f /s");
			if (auto _ttMeltRate = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("melt_emitter_rate_tooltip"), "Depth melted per second at the bowl core. At 1.0 the core reaches full depth in a second; low values make the deepening easy to watch."));

			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx(T(TKEY("debug_cat_object_snow"), "Object Snow"))) {
			{
				// The shaders' StaticsDebugView values; 6 and 7 are unassigned.
				static constexpr int kStaticsDebugModeValues[] = { 0, 1, 2, 3, 4, 5, 8, 9, 10 };
				const char* staticsDebugModes[] = { "Off", "Edge taper", "Coverage alpha", "Normals", "Self-shadow march", "Projected mask", "Skin identity", "Depth fight", "Volume seed" };
				static_assert(IM_ARRAYSIZE(kStaticsDebugModeValues) == IM_ARRAYSIZE(staticsDebugModes));
				int staticsDebugIndex = 0;
				for (int i = 0; i < IM_ARRAYSIZE(kStaticsDebugModeValues); i++)
					if (kStaticsDebugModeValues[i] == staticsDebugView)
						staticsDebugIndex = i;
				if (ImGui::Combo(T(TKEY("statics_debug_view"), "Debug View"), &staticsDebugIndex, staticsDebugModes, IM_ARRAYSIZE(staticsDebugModes)))
					staticsDebugView = kStaticsDebugModeValues[staticsDebugIndex];
				if (auto _ttDbgView = Util::HoverTooltipWrapper())
					ImGui::Text("%s", T(TKEY("statics_debug_view_tooltip"), "Paints the object snow with the decision data behind it instead of its material.\n\nEVERY mode shows the whole snow shell, including the parts normally hidden - an object reading as one solid colour is the view working, not a problem with the snow.\n\nSkin identity: every snow skin in its own colour, striped where the skin is drawn in the game's decal depth mode (the overlay meshes on rocks and trims). The object's own material showing through where snow should be means NO skin won that pixel's depth test - the skin is either drawn and losing, or not drawn at all (see the culling census).\n\nDepth fight: the same test, scored. Green = a skin won the pixel, red = this skin lost it; brightness = by how many depth ULPs (dim = a hair, bright = a lot), striped as above. Green here must coincide with colour in Skin identity; a pixel coloured there but bare here means the view's bias formula is off - report it.\n\nVolume seed (needs Volume Snow on): why the volume does or does not grow on this surface, judged the way the seed pass judges it at the nearest level. GREEN = the volume holds snow here. RED = something captured sits within the headroom above it (a beam, an eave, the member's own inside). BLUE = the surface faces too steeply for Volume Max Slope. YELLOW = seeded but weak - a door, a sheltering roof, or a closed sky; brightness is the weight. MAGENTA = seeded fine, yet no field here - the sweep, rounding or coverage threshold removed it, or a coarser ring owns this spot. Light GREY = this surface is not in the volume's occupancy at all (never captured, or thinner than a voxel). Dark grey = outside the volume's window."));
				ImGui::Checkbox(T(TKEY("debug_proj_albedo"), "Recolor Sampled Albedo"), &debugProjAlbedoView);
				if (auto _ttProjAlbedo = Util::HoverTooltipWrapper())
					ImGui::Text("%s", T(TKEY("debug_proj_albedo_tooltip"), "Paints classified projected snow with the snow texture the recolor JUST SAMPLED, raw and unshaded, in place of blending it in. The magenta view cannot answer this: it replaces the sample with a constant, so it proves the block runs and its weight, and nothing about the texture. Snow here means the sample is good and the recolor is failing further on; the object's own rock or black means the shell snow texture is not reaching the draw."));

				ImGui::Checkbox(T(TKEY("debug_proj_weight"), "Recolor Weight Tint"), &debugProjWeightView);
				if (auto _ttWeightDbg = Util::HoverTooltipWrapper())
					ImGui::Text("%s", T(TKEY("debug_proj_weight_tooltip"), "Paints the game's projected snow with the weight the recolor really blends by, black = none to white = solid; projected surfaces the recolor does not treat as snow turn red. Turn object snow off and compare with the Object Snow Debug View's Projected mask mode (its red channel is the skin's own reconstruction of the same weight): wherever the two disagree is where the shell or its coat paints what the game does not."));
				ImGui::Checkbox(T(TKEY("debug_proj_snow"), "Projected Snow Match Tint"), &debugProjSnowView);
				if (auto _ttProjDbg = Util::HoverTooltipWrapper())
					ImGui::Text("%s", T(TKEY("debug_proj_snow_tooltip"), "Tints every pixel the projected-snow match classifies and replaces in magenta. If a snowy rock or fence shows no magenta, the classification missed that draw; if the magenta area is wrong, the projection weight is. The counters below break last frame's draws down; every projected material record seen is also logged to CommunityShaders.log."));
				// Height-field probe: the seven object maps under the player's
				// feet, one frame old. The numbers behind every layer/height
				// question - tops per peeled layer, the depth cones, and the
				// bridged surface. Sentinels (no data) print as '-'. Showing it
				// keeps the cone built (RoadMapsWanted).
				heightProbeShownFrame = globals::state->frameCount;
				if (probeValid) {
					auto fmtHeight = [](float v, char* out, size_t n) {
						if (v < -50000.0f || v > 50000.0f)
							snprintf(out, n, "-");
						else
							snprintf(out, n, "%.0f", v);
					};
					char l1[16], l2[16];
					fmtHeight(probeVals[0], l1, sizeof(l1));
					fmtHeight(probeVals[1], l2, sizeof(l2));
					char probeLine1[160], probeLine2[160];
					snprintf(probeLine1, sizeof(probeLine1), "Probe @ player z %.0f | layer tops: L1 %s  L2 %s", probeWorldPos.z, l1, l2);
					snprintf(probeLine2, sizeof(probeLine2), "cone depth: L1 %.1f", probeVals[2]);
					WrapText(probeLine1);
					WrapText(probeLine2);
					// Water window under the player: '-' = no water drawn there since
					// the last cell crossing. The cut fires where terrain z < water z.
					char wz[16];
					fmtHeight(probeVals[3], wz, sizeof(wz));
					char probeLine3[160];
					snprintf(probeLine3, sizeof(probeLine3), "water window: z %s | bodies listed %u, drawn %u", wz, statWaterCaptured, statWaterDrawn);
					WrapText(probeLine3);
					static const char* cameraStates[] = { "hook never ran", "no shell data under it", "clear", "pulled in", "camera state not clamped", "off / player not loaded", "lifted (player down)" };
					char probeLine4[192];
					snprintf(probeLine4, sizeof(probeLine4), "camera: z %.0f | snow surface %.0f | %s | hook calls %u, camera state %u%s", cameraProbeZ, cameraProbeSurface, cameraStates[std::min<uint8_t>(cameraProbeState, 6)], cameraProbeFired, cameraProbeCamState, cameraProbeDown ? ", player down" : "");
					WrapText(probeLine4);
				}
				if (auto _ttSdv = Util::HoverTooltipWrapper())
					ImGui::Text("%s", T(TKEY("statics_debug_modes_tooltip"), "Object snow renders its decision data as colors with dithering disabled; missing pixels mean the geometry itself is absent. The trench patch always reads red = trample, green = skin depth (dim) plus the road bit (bright green, above half, means the road owns this column). The skins follow the selected mode. Edge taper: red = the height the taper allows, green = up-facing, blue = the raster returned no data. Coverage alpha: red = the opacity the dither sees, green = the facing gates, blue = the seam blends. Normals: red = smoothed normal z (0.5 = horizontal, 1 = straight up), green = the flat/rounded class. Self-shadow march (patch and skins alike): red = how much the march darkens the pixel, green = taps that rebuilt the road's carved surface, blue = taps that used the flat dusting, dim magenta = the march never ran here (already shadowed, or the sun too low). Projected mask (skins only, patch renders dim gray): red = the skin's own reconstruction of the game's projected-snow blend (hold it against Recolor Weight Tint with object snow off), green = how much snow the mesh's authored data wants - GRADED, so dim green means a dusting and bright green means full snow (zeroed when the draw has no projected-UV data). Yellow = agree, red-only = we place snow where the data says bare, blue = no projection data, magenta = no data but our mask fires."));
			}


			WrapTextDisabledF("Cone passes run this frame: %u of 8", conePassesCoarse);
			WrapTextDisabledF("Roads captured this frame: %u", capturedRoads);
			{
				// The prime worker owns the patch shader members until the object snow group is published.
				const bool primed = !SnowShadersPending(2);
				const char* patchShading = staticsDebugView != 0 ? "forward (debug view)" :
				                           !primed               ? "forward until its shaders compile" :
				                           (patchDeferredPS[0] && patchDeferredPS[1]) ? "deferred" :
				                           PatchDeferredCompiling() ? "forward until its shaders compile" :
				                           (patchDeferredTried[0] || patchDeferredTried[1]) ? "forward (shaders failed to compile)" :
				                                                                              "forward until its shaders compile";
				WrapTextDisabledF("Trench patch shading: %s", patchShading);
			}

			ImGui::Checkbox(T(TKEY("debug_log_all_journeys"), "Log All Capture Verdicts"), &debugLogAllJourneys);
			if (auto _ttJourneys = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("debug_log_all_journeys_tooltip"), "Writes every object's capture verdict to CommunityShaders.log as 'ice journey' lines - mesh name, flags, texture path and which gate accepted or rejected it - once per object and outcome while ticked. Swaying trees and distant LOD are left out. Tick, look at the object in question, untick; then search the log for its mesh name. An object with no line at all never reached the capture."));

			{
				const uint32_t total = skinCullDrawnLast + skinCullCulledLast;
				WrapTextF("Skins: %u drawn, %u culled of %u (%.0f%% skipped)",
					skinCullDrawnLast, skinCullCulledLast, total,
					total ? 100.0 * skinCullCulledLast / total : 0.0);
				WrapTextF("  clusters: %u skins cut, %.2f M of %.2f M triangles drawn, scratch %.2f M indices",
					clusterSkinsLast, skinCullTrisDrawnLast / 1e6, skinCullTrisTotalLast / 1e6, clusterScratchUsedLast / 1e6);
				WrapTextF("  kept: %u tested, %u at eye plane (box %u, giant box %u, sphere %u), %u zero read | culled: %u outside view, %u past far, %u behind scene, %u past range | HiZ top %.5f",
					skinCullReasonLast[0], skinCullReasonLast[1] + skinCullReasonLast[6] + skinCullReasonLast[7],
					skinCullReasonLast[1], skinCullReasonLast[7], skinCullReasonLast[6], skinCullReasonLast[2],
					skinCullReasonLast[3], skinCullReasonLast[4], skinCullReasonLast[5], skinCullReasonLast[8],
					skinCullHiZTopLast);
				if (ImGui::Button(T(TKEY("skin_cull_log"), "Log Skin Cull Verdicts")))
					skinCullLogArmed = true;
				if (auto _ttCullLog = Util::HoverTooltipWrapper())
					ImGui::Text("%s", T(TKEY("skin_cull_log_tooltip"), "Writes one frame's culling verdicts to CommunityShaders.log: every skin the cull skipped, and every skin drawn in the game's decal depth mode, with its mesh name, reason and index count. Use it when the Skin identity view shows an object with no skin at all."));
			}


			WrapTextF("Projected match, last frame: %u classified / %u no projection / %u vetoed",
				statProjMatchedPrev, statProjNoProjectionPrev, statProjVetoedPrev);


			WrapTextF("Snow statics captured: %u", statCapturedStatics.load(std::memory_order_relaxed));
			WrapTextF("Snow Follows Parallax, last frame: %u captures with a height map, %u drapes on the parallax coat%s",
				parallaxCapturesLast, parallaxCoatDrawsLast, (settings.SnowFollowsParallax && coatParallaxTried && !staticsVSParallax) ? " (SHADERS FAILED TO COMPILE)" : "");
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx(T(TKEY("debug_cat_accumulation"), "Snow Accumulation"))) {
			WrapTextF("Snow freshness: %.3f live (0 firm .. 1 fresh), softness applied %.3f", snowFreshness.load(std::memory_order_relaxed), GetSnowSoftness());
			ImGui::SliderFloat(T(TKEY("freshness_override"), "Snow Freshness Override"), &debugFreshnessOverride, -1.0f, 1.0f, debugFreshnessOverride < 0.0f ? "live" : "%.2f");
			if (auto _ttFreshOv = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("freshness_override_tooltip"), "Not saved. Below 0 the weather drives the snow's freshness; at 0 or above this value replaces it, so firm (0) and fresh (1) can be compared on the spot without waiting for a storm. The ridge and rim change at once; wall steepness only shows on trenches cut after the change."));

			{
				// The rate is spelled out because the exit test is "does the number
				// do what the plan's table says", which needs the arithmetic
				// visible rather than inferred from watching it drift.
				const float accum = snowAccumulation.load(std::memory_order_relaxed);
				const float intensity = accumWeatherIntensity.load(std::memory_order_relaxed);
				const float growth = settings.AccumulationHours > 0.01f ? intensity / settings.AccumulationHours : 0.0f;
				const float melt = settings.AccumulationMeltHours > 0.01f ? (1.0f - intensity) / settings.AccumulationMeltHours : 0.0f;
				const float fade = settings.AccumulationFadeDays > 0.01f ? 1.0f / (settings.AccumulationFadeDays * 24.0f) : 0.0f;
				const float rate = growth - melt - fade;

				auto* tes = globals::game::tes;
				const char* held = !tes ? "" : tes->interiorCell ? " (held, indoors)" : NoSkyWorldspace() ? " (held, no sky)" : "";

				// The scale the shells are actually handed, not what the peak would
				// give: with the toggle off it reads x1.000, which is the other
				// half of the A/B saying so.
				WrapTextF("Accumulation: %.3f (depth x%.3f%s), snowfall %.2f%s",
					accum, GetAccumulationDepthScale(),
					settings.EnableSnowAccumulation ? "" : ", not applied", intensity, held);
				WrapTextF("Rate: %+.4f/game hour (grow %.4f, melt %.4f, fade %.4f)",
					rate, growth, melt, fade);
				WrapTextF("Clock: %.2f game hours, timescale %.0f",
					gameClock.lastHours, gameClock.timescale);
				WrapTextF("Co-save: %s", settings.PersistAccumulation ? "remembered" : "not written");
			}


			WrapTextF("Snowfall intensity: %.2f (refill %s)", snowfallIntensity,
				settings.RefillOnlyWhenSnowing ? "weather-driven" : "baseline");

			{
				static const char* kSnowGateNames[] = {
					"running (snowy cell in reach)",
					"running (unbaked ground in reach, assumed snow)",
					"suspended (all ground in reach known bare)"
				};
				WrapTextF("Snow presence gate (loaded square): %s, bare for %u/%u frames%s", kSnowGateNames[std::min(deformSnowVerdict, 2u)], deformGateBareFrames, kShellGateBareFrames, deformSuspended ? " - SUSPENDED" : "");
				WrapTextF("Shell gate (snow in view): %s, bare for %u/%u frames", kSnowGateNames[std::min(shellSnowVerdict, 2u)], shellGateBareFrames, kShellGateBareFrames);
			}


			if (auto* sky = globals::game::sky)
				WrapTextF("Wind: %.2f toward %.0f deg (drift-biased refill)", sky->windSpeed,
					Util::Units::RadiansToDegrees(sky->windAngle));
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx(T(TKEY("debug_cat_condition"), "Snow Condition"))) {
			// Diagnostics use plain text by existing convention (no i18n).
			const SnowCondition condition = GetSnowCondition();
			const auto& climate = snowClimate;
			auto* tes = globals::game::tes;
			const bool indoors = !tes || tes->interiorCell || NoSkyWorldspace();
			static const char* kHardness[] = { "F", "4F", "1F", "P", "K" };
			static const char* kWetClass[] = { "dry", "moist", "wet", "very wet", "slush" };
			const char* hardness = kHardness[(condition.density >= 120.0f) + (condition.density >= 220.0f) + (condition.density >= 320.0f) + (condition.density >= 420.0f)];
			const char* wetClass = kWetClass[(condition.wetness >= 0.15f) + (condition.wetness >= 0.4f) + (condition.wetness >= 0.7f) + (condition.wetness >= 0.9f)];
			WrapTextF("Temperature at the player: %+.1f C%s | target %+.1f = land %+.1f, weather %+.1f, sun %+.1f%s",
				condition.temperature, debugClimateTemperatureOn ? " (override)" : "", climate.targetTemperature,
				climate.termRegion, climate.termWeather, climate.termSun, indoors ? " | held indoors" : "");
			if (climate.landSnowShare < 0.0f)
				WrapTextF("Land snow share: not measured yet (using 0.50)%s", debugClimateShareOverride >= 0.0f ? ", override applied" : "");
			else
				WrapTextF("Land snow share: %.2f%s", climate.landSnowShare, debugClimateShareOverride >= 0.0f ? ", override applied" : "");
			WrapTextF("Wetness: %.2f, %s%s", condition.wetness, wetClass, debugClimateWetnessOverride >= 0.0f ? " (override)" : "");
			WrapTextF("Meltwater held (2 C colder / here / 2 C warmer): %.2f / %.2f / %.2f | ice %.2f / %.2f / %.2f%s%s", climate.rowWater[0], climate.rowWater[1],
				climate.rowWater[2], climate.rowIce[0], climate.rowIce[1], climate.rowIce[2], debugClimateIceOverride >= 0.0f ? " (shown: override)" : "",
				settings.WetSnowAndIce ? "" : " | Wet Snow and Ice is off");
			WrapTextF("Stickiness: %.2f%s", condition.stickiness, debugClimateStickinessOverride >= 0.0f ? " (override)" : "");
			WrapTextF("Firmness: %.2f%s | density %.0f kg/m3, hand hardness %s", condition.firmness,
				debugClimateFirmnessOverride >= 0.0f ? " (override)" : "", condition.density, hardness);
			WrapTextF("Freshness %.2f | wind packing %.2f | snowfall %.2f | rain %.2f", condition.freshness, condition.windPacking,
				accumWeatherIntensity.load(std::memory_order_relaxed), climate.rainHeld);
			WrapTextF("Ageing clock: %.2f, running at %.1f per game hour%s", climate.ageingClock, climate.ageingRate,
				debugAgeingRateOverride >= 0.0f ? " (override)" : "");
			if (const float ageing = std::clamp(settings.TrackAgeing, 0.0f, 2.0f); ageing <= 0.0f)
				WrapTextF("Tracks: never age (Track Ageing 0)");
			else if (climate.ageingRate <= 0.0f)
				WrapTextF("Tracks: the clock is stopped, nothing ages");
			else
				WrapTextF("Tracks: fully aged after %.1f game hours at this rate (Track Ageing %.2f, %.0f clock units)",
					kTrackAgeSpan / (ageing * climate.ageingRate), ageing, kTrackAgeSpan / ageing);
			{
				const SnowClimateLook look = ComputeSnowClimateLook();
				WrapTextF("Climate look x Snow Climate %.2f: berm %.2f, lip %.2f, teeth %.2f, wall slough %.2f", look.strength,
					look.berm, look.lip, look.teeth, look.slough);
			}

			ImGui::Checkbox(T(TKEY("climate_temperature_override_on"), "Override Temperature"), &debugClimateTemperatureOn);
			if (auto _ttTempOn = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("climate_temperature_override_on_tooltip"), "Not saved. Replaces the temperature at the player; wetness, stickiness and firmness follow it, and higher ground still reads colder."));
			if (debugClimateTemperatureOn)
				ImGui::SliderFloat(T(TKEY("climate_temperature_override"), "Temperature"), &debugClimateTemperature, -30.0f, 10.0f, "%+.1f C");
			ImGui::SliderFloat(T(TKEY("climate_share_override"), "Land Snow Share Override"), &debugClimateShareOverride, -1.0f, 1.0f, debugClimateShareOverride < 0.0f ? "live" : "%.2f");
			if (auto _ttShare = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("climate_share_override_tooltip"), "Not saved. Below 0 the land around the player decides how cold the region is: all snow reads -12 C, no snow +4 C, before weather and sun."));
			ImGui::SliderFloat(T(TKEY("climate_wetness_override"), "Wetness Override"), &debugClimateWetnessOverride, -1.0f, 1.0f, debugClimateWetnessOverride < 0.0f ? "live" : "%.2f");
			ImGui::SliderFloat(T(TKEY("climate_stickiness_override"), "Stickiness Override"), &debugClimateStickinessOverride, -1.0f, 1.0f, debugClimateStickinessOverride < 0.0f ? "live" : "%.2f");
			ImGui::SliderFloat(T(TKEY("climate_firmness_override"), "Firmness Override"), &debugClimateFirmnessOverride, -1.0f, 1.0f, debugClimateFirmnessOverride < 0.0f ? "live" : "%.2f");
			if (auto _ttCond = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("climate_condition_override_tooltip"), "Not saved. Below 0 the value is derived; at 0 or above it replaces it at the player and in the view."));
			ImGui::SliderFloat(T(TKEY("climate_ice_override"), "Ice Override"), &debugClimateIceOverride, -1.0f, 1.0f, debugClimateIceOverride < 0.0f ? "live" : "%.2f");
			if (auto _ttIce = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("climate_ice_override_tooltip"), "Not saved. Below 0 the ice is what the weather has frozen; at 0 or above it replaces it everywhere, without touching the meltwater the snow holds."));
			ImGui::SliderFloat(T(TKEY("ageing_rate_override"), "Ageing Clock Rate Override"), &debugAgeingRateOverride, -1.0f, 60.0f, debugAgeingRateOverride < 0.0f ? "live" : "%.1f per hour");
			if (auto _ttAge = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("ageing_rate_override_tooltip"), "Not saved. How fast tracks age, in clock units per game hour: about 1 in calm cold weather, over 20 while it snows. At Track Ageing 1 a track is fully aged after 24 units."));

			{
				const char* views[] = { "Off", "Temperature", "Wetness", "Stickiness", "Firmness", "All (wet R, sticky G, firm B)", "Track age (green new, red old)", "Ice (cyan glaze)" };
				ImGui::Combo(T(TKEY("condition_view"), "Snow Condition View"), &debugConditionView, views, IM_ARRAYSIZE(views));
				if (auto _ttView = Util::HoverTooltipWrapper())
					ImGui::Text("%s", T(TKEY("condition_view_tooltip"), "Paints the ground snow with the condition at each point: higher ground is colder. Track age paints trenches and their berms from green, just cut, to red, fully aged; untouched snow stays white. Ice paints the glaze on packed snow cyan. Roads and objects keep their look."));
				if (debugConditionView > 0) {
					static const char* kViewStates[] = { "starting", "compiling", "ready", "failed to compile" };
					WrapTextF("View shader: %s", kViewStates[std::clamp(conditionViewShaderState.load(std::memory_order_acquire), 0, 3)]);
				}
			}

			{
				uint32_t keyed = 0, nearFull = 0, farFull = 0;
				for (uint32_t i = 0; i < marchCensusFilled; i++) {
					keyed += marchCensus[i] & 1u;
					nearFull += (marchCensus[i] >> 1) & 1u;
					farFull += (marchCensus[i] >> 2) & 1u;
				}
				WrapTextF("Sun march cache, last %u updates: rebuilt whole %u near, %u far (settings key changed %u)",
					marchCensusFilled, nearFull, farFull, keyed);
			}
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx("Weight sink")) {
			itemSinkReadoutWanted.store(true, std::memory_order_release);
			std::string readout;
			{
				std::scoped_lock lock(itemSinkLock);
				readout = itemSinkReadout;
			}
			WrapTextF("dropped items in range %u (within %.0f units, nearest %u) | held above the ground %u",
				itemSinkWatched.load(std::memory_order_relaxed), kItemSinkRadius, (uint32_t)kItemSinkMax, itemSinkHeld.load(std::memory_order_relaxed));
			WrapTextF("%s", readout.c_str());
			WrapTextF("Each item type logs its measure and roundness once to CommunityShaders.log ('item sink:').");
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx("Creature weight")) {
			creatureSinkReadoutWanted.store(true, std::memory_order_release);
			std::string readout;
			{
				std::scoped_lock lock(creatureLiftLock);
				readout = creatureSinkReadout;
			}
			WrapTextF("creatures watched %u | lifted %u | %.0f us last frame", creatureSinkWatched.load(std::memory_order_relaxed),
				creatureSinkLifted.load(std::memory_order_relaxed), creatureSinkMicros.load(std::memory_order_relaxed));
			WrapTextF("%s", readout.c_str());
			WrapTextF("Each creature race logs its weight once to CommunityShaders.log ('creature weight:').");
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx("Snow Events")) {
			ImGui::Checkbox("Count Snow Events", &debugSnowEventCensus);
			if (auto _ttSnowEvents = Util::HoverTooltipWrapper())
				ImGui::Text("%s", "Not saved. Collects what a particle feature would read (MD plans/SSPP-SNOW-PLAN.md, stage E0): feet kicking snow and prints crumbling, landings, knockdowns, dodges, bodies dropping, and shouts, blasts and lightning that move snow. Off, nothing is collected.");
			for (const auto& line : SnowEventCensusLines())
				WrapTextF("%s", line.c_str());
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx("Snow Coat Aging")) {
			for (const auto& line : CoatAgingLines())
				WrapTextF("%s", line.c_str());
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx("Snow on Bodies (S2 Look)")) {
			ImGui::Combo("Force Coat", &debugBodyCoatForce, "Off\0Legs to the Knee\0Snowfall from Above\0Left Half\0Everything\0");
			if (auto _ttForce = Util::HoverTooltipWrapper())
				ImGui::Text("%s", "Not saved. Stage S2 of MD plans/BODY-SNOW-PLAN.md: until contact and snowfall write a real coat, puts a synthetic one on the player and the actors near them, so the look can be judged. Legs to the Knee: contact snow below each actor's knees. Snowfall from Above: fallen snow, shown only on the parts that face up in the rest pose. Left Half: contact snow down the left side, as on a body that lay there. Everything: the whole body. The coat wears the snow shell's own texture set (colour, grain, roughness, brightness). Skin, eyes, brows and scars take none. The first time it turns on, the coat's shaders compile, so armour and clothing take a few seconds to show it.");
			ImGui::SliderFloat("Force Coat Amount", &debugBodyCoatAmount, 0.0f, 1.0f, "%.2f");
			if (auto _ttAmount = Util::HoverTooltipWrapper())
				ImGui::Text("%s", "Not saved. How much snow the forced coat holds: low is a dusting the armour shows through, 1 is a full coat whose clumps go solid.");
			ImGui::SliderFloat("Angle of Repose", &debugBodyCoatRepose, 20.0f, 85.0f, "%.0f deg");
			if (auto _ttRepose = Util::HoverTooltipWrapper())
				ImGui::Text("%s", "Not saved. The slope where snowfall starts sliding off: flatter surfaces hold all of it, and it is gone 20 degrees steeper. The armour's own relief counts, so the walls of its folds and cracks shed snow first.");
			ImGui::SliderFloat("Graininess", &debugBodyCoatGrain, 0.0f, 1.0f, "%.2f");
			if (auto _ttGrain = Util::HoverTooltipWrapper())
				ImGui::Text("%s", "Not saved. How far loose grains of snow spread past the clumps' edges and, for snowfall, past the slope where it slides off. 0: clumps only. 1: a wide halo of grains.");
			ImGui::Combo("Coat Look", &debugBodyCoatLook, "Crust\0Flat Paint\0");
			if (auto _ttLook = Util::HoverTooltipWrapper())
				ImGui::Text("%s", "Not saved. S2's A/B. Crust: snow lies in solid clumps over a relief of its own, their number set by the coverage, with loose grains around them that thin out with distance and slope; it never fades to a grey film. Flat Paint: one even layer whose opacity equals the coverage, for comparison.");
			ImGui::Combo("Body Coat View", &debugBodyCoatView, "Off\0Canonical Bands\0Slot Pattern\0Slots and Frames\0");
			if (auto _ttView = Util::HoverTooltipWrapper())
				ImGui::Text("%s", "Not saved. Stage S1 of MD plans/BODY-SNOW-PLAN.md: paints every mapped skin of the player and the actors near them, so the coat's frames can be judged before any snow is written. Canonical Bands: coloured bands every 8 units of height and white lines every 16 across, in each actor's own rest frame; they must stay glued to the body and run unbroken from body to armour, boots, gloves and first person. Slot Pattern: the checks each actor's atlas slot holds, read back as the coat will read it; magenta means past the slot's bounds. Slots and Frames: one colour per slot, striped on a skin in a frame of its own (a bow, a tail). A view replaces the forced coat's look. Off, with Force Coat off too, empties the store.");
			ImGui::Checkbox("Run Body Census", &debugBodyCoatCensus);
			if (auto _ttCensus = Util::HoverTooltipWrapper())
				ImGui::Text("%s", "Not saved. Every 2 s, measures the actors the body coat covers: each skin mapped into its actor's frame through the bones it shares, frames of their own for skins that share none, carried gear, first-person meshes, and whether each mesh names its actor. Eyes, brows, scars, refraction effects and engine decals are left out. With Body Coat View on, it reports the frames, slots and records the coat uses. Reads only.");
			ImGui::SameLine();
			if (ImGui::Button("Write Body Census to Log"))
				debugBodyCoatCensusDump = true;
			if (auto _ttDump = Util::HoverTooltipWrapper())
				ImGui::Text("%s", "Runs the census once now and writes every actor, mesh and carried item to CommunityShaders.log ('body coat census:').");
			ImGui::Checkbox("Probe Lighting Binds", &debugBodyCoatDrawProbe);
			if (auto _ttProbe = Util::HoverTooltipWrapper())
				ImGui::Text("%s", "Not saved. Once a frame, binds a 1-texel probe texture at t41 (the coat atlas itself while Body Coat View is on) and counts the actor draws it still reaches, by pass: deferred, forward, reflections. Also counts which draws name their actor. One log line per 300 frames ('body coat draw probe').");
			for (const auto& line : BodyCoatProbeLines())
				WrapTextF("%s", line.c_str());
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx(T(TKEY("debug_cat_spells"), "Spell Integration"))) {
			if (ImGui::TreeNodeEx(T(TKEY("spell_cat_stats"), "Detected"))) {
				// Diagnostics use plain text by existing convention (no i18n).
				WrapTextF("projectiles %u | streams %u | hazards %u | cloaks %u | ground hits %u | trails %u",
					spellStats.projectiles, spellStats.streams, spellStats.hazards, spellStats.auras,
					spellStats.groundContacts, spellStats.trails);
				WrapTextF("blasts: armed %u | detonations %u | casts %u | shouts %u (%u discs)   [totals since load]",
					spellStats.armed, spellStats.detonations, spellStats.casts,
					spellStats.shouts, spellStats.shoutDiscs);
				WrapTextF("dash watches %u | furrows cut %u | travelling shoves %u | frost effects raised %u",
					spellStats.dashWatches, spellStats.dashGouges, spellStats.forceTracks, spellStats.lifted);
				if (spellStats.lastShoutVerdict) {
					static const char* kElem[] = { "none", "fire", "frost", "shock", "force" };
					static const char* kVerdict[] = { "-", "WEDGE", "TRACK", "rejected" };
					WrapTextF("last shout: %s | proj speed %.0f | impact force %.0f | %s",
						spellStats.lastShoutElement < IM_ARRAYSIZE(kElem) ? kElem[spellStats.lastShoutElement] : "?",
						spellStats.lastShoutSpeed, spellStats.lastShoutForce,
						spellStats.lastShoutVerdict < IM_ARRAYSIZE(kVerdict) ? kVerdict[spellStats.lastShoutVerdict] : "?");
				}
				WrapTextF("runes in range %u | rune decal draws taken %u | glyph tiles painted %u | spell decals masked over object snow %u",
					runeStatSites, runeStatCaptures, runeStatTiles, decalMasksLast);
				WrapTextF("rejected: no element %u | no blast form %u",
					spellStats.rejectedElement, spellStats.rejectedNoBlast);
				WrapTextF("innate auras %u | bodies burning %u | marking corpses %u | floating %u | translucent %u (neither carving)",
					spellStats.innate, spellStats.burning, spellStats.corpses, stampStats.floating, stampStats.incorporeal);
				WrapTextF("death events seen %u | death blasts opened %u",
					spellStats.deathsSeen, spellStats.deathBlasts);
				if (stampStats.nearestValid) {
					static const char* kStateNames[] = { "on ground", "jumping", "in air", "climbing", "flying", "swimming" };
					const uint state = stampStats.nearestState;
					WrapTextF("nearest actor: %s | gap to its own footing %.0f | gap to land %.0f | state %s",
						stampStats.nearestFloating ? "FLOATING" : "touching",
						stampStats.nearestGapToRoot, stampStats.nearestGapToLand,
						state < IM_ARRAYSIZE(kStateNames) ? kStateNames[state] : "none");
					WrapTextF("             bones: feet %u usable %u%s | dry travel %.0f | limbs %u        (frame totals: feet %u | limbs %u | shapes %u | props %u | failsafe actors %u)",
						stampStats.nearestFeet, stampStats.nearestUsableFeet,
						stampStats.nearestFallback ? " FAILSAFE" : "",
						stampStats.nearestDryTravel, stampStats.nearestLimbs,
						stampStats.feet, stampStats.limbs, stampStats.shapes, stampStats.props,
						stampStats.fallbackActors);
					WrapTextF("             body alpha %s | marked Ghost %s | verdict %s",
						stampStats.nearestElemental ? "not read" :
						                              std::format("{:.2f} on '{}'", stampStats.nearestBodyAlpha, stampStats.nearestBodyAlphaSource).c_str(),
						stampStats.nearestGhostFlag ? "yes" : "no",
						stampStats.nearestIncorporeal ? "INCORPOREAL" :
							(stampStats.nearestElemental ? "solid (made of an element)" : "solid"));
				}
				WrapTextF("emitters %u | awaiting their step %u | last mark: strength %.2f radius %.0f",
					spellStats.emitters, spellStats.pending, spellStats.lastStrength, spellStats.lastRadius);
				WrapTextF("budget: actors+props %u/%u | spells %u/%u | emitters culled by distance %u | actors turned away %u",
					stampStats.beforeSpells, kMaxStamps - kSpellStampReserve,
					stampStats.spells, kSpellStampReserve, spellStats.emittersCulled,
					stampStats.budgetTurnedAway);
				ImGui::TreePop();
			}

			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx(T(TKEY("debug_cat_shell_perf"), "Shell Shader Performance"))) {
			ImGui::TextDisabled("%s", shellDeferredThisFrame ? T(TKEY("shell_deferred_active"), "Deferred pass: drawing") : T(TKEY("shell_deferred_inactive"), "Deferred pass: not drawing"));
			const char* joinState = T(TKEY("shell_join_off"), "Depth prepass join: off");
			switch (shellEarlyState) {
			case EarlyPrepass::Drawing:
				joinState = T(TKEY("shell_join_drawing"), "Depth prepass join: drawing");
				break;
			case EarlyPrepass::NotDrawing:
				joinState = T(TKEY("shell_join_not_drawing"), "Depth prepass join: not this frame (no ground snow or a new render size)");
				break;
			case EarlyPrepass::NeedsTerrainBlending:
				joinState = T(TKEY("shell_join_needs_tb"), "Depth prepass join: needs Terrain Blending");
				break;
			case EarlyPrepass::Unavailable:
				joinState = T(TKEY("shell_join_unavailable"), "Depth prepass join: unavailable (a shader failed, or no landscape capture this frame)");
				break;
			default:
				break;
			}
			ImGui::TextDisabled("%s", joinState);
			if (shellEarlyViewMismatches > 0)
				ImGui::TextDisabled("%s %u", T(TKEY("shell_join_view_mismatch"), "Frames whose view changed under the early pass:"), shellEarlyViewMismatches);
			// The menu draws after Present has advanced the frame counter.
			const bool sssSynced = shellSssSyncFrame != UINT32_MAX && globals::state->frameCount - shellSssSyncFrame <= 1u;
			ImGui::TextDisabled("%s", sssSynced ? T(TKEY("shell_sss_drawing"), "Shadow depth sync: drawing") :
			                                       T(TKEY("shell_sss_not_drawing"), "Shadow depth sync: not this frame (needs the depth prepass join drawing, Snow Contact Shadows, and Screen-Space Shadows in an exterior)"));
			const bool skinSssMarched = skinSssMarchFrame != UINT32_MAX && globals::state->frameCount - skinSssMarchFrame <= 1u;
			ImGui::TextDisabled("%s", skinSssMarched ? T(TKEY("skin_sss_drawing"), "Object snow shadow march: drawing") : T(TKEY("skin_sss_not_drawing"), "Object snow shadow march: not this frame (needs object snow on screen with its depth prepass, Snow Contact Shadows, and Screen-Space Shadows in an exterior)"));
			ImGui::TreePop();
		}
		ImGui::TreePop();
	}
#endif
}
