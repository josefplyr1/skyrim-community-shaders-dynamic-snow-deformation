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
		{ 2048u, 1024u, 115.0f, 105.0f, 150.0f, 0.25f, 0.13f, 24.0f, false, false, true, false, true },
		{ 4096u, 2048u, 200.0f, 200.0f, SnowDeformation::kMaxRangeSkinsM, 0.5f, 0.25f, 16.0f, true, true, true, true, true },
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
			settings.SkinTessCapPx == p.skinTessCapPx &&
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

		ImGui::SliderFloat(T(TKEY("seasons_snow_max_angle"), "Seasons Snow Max Angle"), &settings.SeasonsSnowMaxAngle, 0.0f, 90.0f, "%.0f deg");
		if (auto _ttSeasons = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("seasons_snow_max_angle_tooltip"), "Steepest slope that keeps snow on objects Seasons of Skyrim paints at runtime. Those projections come with the record default of 90 degrees and no painted mask; 90 keeps Seasons' own coverage, which the recolor and the drape then follow exactly, and lower cuts the snow off steeper faces for them only. Authored snow objects keep their own angle and mask."));

		{
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

		ImGui::SliderFloat(T(TKEY("skin_depth_bias"), "Object Snow Depth Bias"), &settings.SkinDepthBias, 0.0f, 128.0f, "%.0f");
		if (auto _ttSdb = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("skin_depth_bias_tooltip"), "How far the object snow layer is nudged toward the camera in the depth buffer, in the buffer's own smallest steps. The layer is rasterised from the same vertices as the object under it, so at a distance the two land on the same depth value and fight - snow that flickers, or shows from one angle and not another. A few steps settle the fight; more than needed makes the layer stand in front of things that should hide it. Constant with distance in depth terms, which is what the contest needs."));

		ImGui::SliderFloat(T(TKEY("skin_slope_depth_bias"), "Object Snow Slope Bias"), &settings.SkinSlopeDepthBias, 0.0f, 4.0f, "%.2f");
		if (auto _ttSsb = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("skin_slope_depth_bias_tooltip"), "The part of the depth bias that grows with how steeply a face is seen. Faces at a grazing angle cover a large depth range per pixel and fight hardest; this scales the nudge with that slope. Capped internally so no angle can push the layer far in front of its object."));

		ImGui::SliderFloat(T(TKEY("skin_tess_cap_px"), "Object Snow Tessellation Cap"), &settings.SkinTessCapPx, 0.0f, 32.0f, "%.0f px");
		if (auto _ttStc = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("skin_tess_cap_px_tooltip"), "Caps how finely road snow is subdivided (no other object snow is): no subdivision segment may be smaller on screen than this many pixels. At a distance, finer segments end up smaller than a pixel and cost GPU time for nothing visible. Higher = cheaper: up to about 20 it only trims the rounded edges at range; above that it coarsens the whole road surface. 0 turns the cap off. The Medium and Low presets raise it."));

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

		ImGui::SeparatorText(T(TKEY("trampled_group"), "Trampled Ground"));

		ImGui::SliderFloat(T(TKEY("trampled_flattening"), "Flattening"), &settings.TrampledFlattening, 0.0f, 1.0f, "%.2fx");
		if (auto _ttTrampleFlat = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("trampled_flattening_tooltip"), "Snow walked over again and again is packed down, and the broken snow on the trench floor is walked flatter: this multiplies Detail Relief on floors deeper than one pass leaves them, easing in the deeper the traffic has packed them. At 0.50x a Detail Relief of 1 becomes 0.5. The berms and a trail walked once are never touched. 1.00x = off."));

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

#if !SNOW_ALPHA_BUILD
	if (ImGui::TreeNodeEx(T(TKEY("debug_options"), "Debugging Options"), ImGuiTreeNodeFlags_Framed)) {
		if (ImGui::TreeNodeEx(T(TKEY("debug_cat_contact"), "Actor & Prop Contact"))) {
			WrapTextF("  actors: %u living + %u corpses rasterized (caps %u / %u), %u partition draws + %u carried meshes (+%u sweep sub-steps; bone stamps skipped for these), %u overlays + %u fur shells declined, %u settled corpses latched out, %u hidden/orphan partitions declined, %u whole-skin-indexed, %u missing-bone stand-ins",
				stampStats.actorsRasterized, stampStats.corpsesRasterized, kContactMaxActors, kContactMaxCorpses, contactSkinDrawsLast, contactCarriedLast, contactSweepLast, contactOverlaysLast, contactShellsLast, stampStats.corpsesLatched, contactHiddenPartsLast, contactGlobalPartsLast, contactSkinMissingLast);

			CheckboxWrapped("Contact field view (what the rasterizer wrote this frame)", &debugContactView);

			if (debugContactView) {
				SliderFloatWrapped("Field view crop (units each side; 192 = a body, 1536 = the whole field)", &debugContactViewHalf, 64.0f, kContactHalfExtent, "%.0f");
				CheckboxWrapped("Object print view (Snow Trenches on Objects: each print texel's verdict left, the print map right)", &debugObjectPrintView);
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
								const float2 boundCenter = contactViewBoundCenter;
								const float boundRadius = contactViewBoundRadius;
								if (boundRadius > 0.0f)
									ImGui::GetWindowDrawList()->AddRect(
										{ toX(boundCenter.x - boundRadius), toY(boundCenter.y + boundRadius) },
										{ toX(boundCenter.x + boundRadius), toY(boundCenter.y - boundRadius) },
										IM_COL32(255, 220, 80, 220), 0.0f, 0, 1.5f);
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
							WrapTextF("player bound: centre (%.0f, %.0f) radius %.0f | yaw %.2f rad | crop centre (%.0f, %.0f) half %.0f, live bound drifts (%.0f, %.0f) | field centre (%.0f, %.0f) | map texel %.2f units, contact texel %g units | dots: feet white, calves grey, hands cyan, head/spine magenta",
								root->worldBound.center.x, root->worldBound.center.y, root->worldBound.radius, player->GetAngleZ(),
								contactViewCenter.x, contactViewCenter.y, contactViewHalfUsed,
								root->worldBound.center.x - contactViewCenter.x, root->worldBound.center.y - contactViewCenter.y,
								contactCenter.x, contactCenter.y,
								contactViewTexelSize, 2.0f * kContactHalfExtent / (float)kContactDim);
					}
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

			WrapTextF("Stamps/frame: feet %u, limbs %u, shapes %u, props %u (refs at last scan %u, movers %u, scan every 6 frames)",
				stampStats.feet, stampStats.limbs, stampStats.shapes, stampStats.props,
				propScanRefs, stampStats.propMovers);
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx(T(TKEY("debug_cat_cpu"), "CPU & Memory"))) {
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

			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx(T(TKEY("debug_cat_deform_map"), "Deformation Map"))) {
			if (ImGui::Button(T(TKEY("clear"), "Clear Map"))) {
				clearRequested = true;
				ClearTrenchStore("the Clear Deformation Map button");
				ClearPrintStore("the Clear Deformation Map button");
			}

			ImGui::Checkbox(T(TKEY("debug_force_tiles"), "Force All Tiles Dirty"), &debugForceAllTilesDirty);

			ImGui::Checkbox(T(TKEY("debug_force_update"), "Force Update"), &debugForceDeformationUpdate);

			ImGui::Checkbox(T(TKEY("show_debug"), "Show Map"), &showDebugTexture);
			if (showDebugTexture) {
				WrapTextF("%s", T(TKEY("debug_hint"), "Deformation map: red = compressed snow. The window is centred on the camera, so the cross is you and the ring is 25 m."));
				const ImVec2 imageTopLeft = ImGui::GetCursorScreenPos();
				if (trenchDebugSRV)
					ImGui::Image(trenchDebugSRV.get(), { 512.0f, 512.0f });
				else if (auto* deformSRV = GetDeformationSRV())
					ImGui::Image(deformSRV, { 512.0f, 512.0f });

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

				if (trenchInjectSRV) {
					WrapTextF("%s", T(TKEY("debug_inject_hint"), "Trench store: what the inject last painted into the window. Opaque, so what you see is what the store holds. Refreshed when the window scrolls or is rebuilt - after a load it is the whole restored window."));
					ImGui::Image(trenchInjectSRV.get(), { 512.0f, 512.0f });
				}
			}

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
				WrapTextF("Evolve pass: %s", evolveIdleLastFrame ? "idle (settled)" : "active");
			}

			if (deformSkipRate >= 0.0f) {
				WrapTextF("Stamp set changes: %u/300 frames (count %u, drift %u)",
					stampSetCountChanges + stampSetDriftChanges, stampSetCountChanges, stampSetDriftChanges);
			}

			{
				const uint32_t totalTiles = (deformMapDim / 8) * (deformMapDim / 8);
				if (debugForceAllTilesDirty)
					WrapTextF("Stamp tiles: forced to full map (%u tiles)", totalTiles);
				else if (stampTilesLast > kStampTileCap)
					WrapTextF("Stamp tiles: OVERFLOWED to full map (%u tiles)", totalTiles);
				else
					WrapTextF("Stamp tiles: %u of %u", stampTilesLast, totalTiles);
				WrapTextF("Evolve tiles: %u of %u", evolveTilesLast, totalTiles);
				WrapTextF("Berm tiles: %u of %u", bermTilesLast, totalTiles);
			}

			{
				const auto stats = GetTrenchStoreStats();
				WrapTextF("Trench store: %zu tiles, %.1f KB raw, %.1f%% full, %zu thin",
					stats.tiles, (double)stats.bytes / 1024.0, stats.occupancy * 100.0f, stats.thin);
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

		if (ImGui::TreeNodeEx(T(TKEY("debug_cat_object_snow"), "Object Snow"))) {
			{
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
				const float accum = snowAccumulation.load(std::memory_order_relaxed);
				const float intensity = accumWeatherIntensity.load(std::memory_order_relaxed);
				const float growth = settings.AccumulationHours > 0.01f ? intensity / settings.AccumulationHours : 0.0f;
				const float melt = settings.AccumulationMeltHours > 0.01f ? (1.0f - intensity) / settings.AccumulationMeltHours : 0.0f;
				const float fade = settings.AccumulationFadeDays > 0.01f ? 1.0f / (settings.AccumulationFadeDays * 24.0f) : 0.0f;
				const float rate = growth - melt - fade;

				auto* tes = globals::game::tes;
				const char* held = !tes ? "" : tes->interiorCell ? " (held, indoors)" : NoSkyWorldspace() ? " (held, no sky)" : "";

				WrapTextF("Accumulation: %.3f (depth x%.3f%s), snowfall %.2f%s",
					accum, GetAccumulationDepthScale(),
					settings.EnableSnowAccumulation ? "" : ", not applied", intensity, held);
				WrapTextF("Rate: %+.4f/game hour (grow %.4f, melt %.4f, fade %.4f)",
					rate, growth, melt, fade);
				WrapTextF("Clock: %.2f game hours, timescale %.0f",
					weatherClock.lastHours, weatherClock.timescale);
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
			WrapTextF("Ice that froze in a track cut 1 / 6 / 24 clock units ago (here): %.2f / %.2f / %.2f", SnowIceSinceCut(1, 1), SnowIceSinceCut(1, 6),
				SnowIceSinceCut(1, 24));
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
				ImGui::Text("%s", T(TKEY("climate_ice_override_tooltip"), "Not saved. Below 0 the ice is what the weather has frozen; at 0 or above it replaces it everywhere, as a freeze that happened when it was set: prints cut in an earlier clock unit glaze, prints cut since do not. It does not touch the meltwater the snow holds."));
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

		if (ImGui::TreeNodeEx("Snow Events")) {
			ImGui::Checkbox("Count Snow Events", &debugSnowEventCensus);
			if (auto _ttSnowEvents = Util::HoverTooltipWrapper())
				ImGui::Text("%s", "Not saved. Collects what a particle feature would read (MD plans/SSPP-SNOW-PLAN.md, stage E0): feet kicking snow and prints crumbling, landings, knockdowns, dodges, bodies dropping, and shouts, blasts and lightning that move snow. Off, nothing is collected.");
			for (const auto& line : SnowEventCensusLines())
				WrapTextF("%s", line.c_str());
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
