#include "Features/SnowDeformation.h"

#include <imgui_stdlib.h>

#include "Utils/UI.h"

#define I18N_KEY_PREFIX "feature.snow_deformation."

void SnowDeformation::DrawSettings()
{
	if (ImGui::TreeNodeEx(T(TKEY("snow_deformation"), "Snow Deformation"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T(TKEY("enable"), "Enable Snow Deformation"), &settings.EnableSnowDeformation);

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
		ImGui::Checkbox(T(TKEY("snow_texture_linear"), "Linear (PBR) Texture"), &settings.SnowTextureLinear);
		if (auto _ttLin = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("snow_texture_linear_tooltip"), "Legacy override: enable when a NON-PBR texture stores linear color. When a PBR set is auto-resolved (Textures\\PBR\\...), linear color is detected automatically and this checkbox is ignored."));

		ImGui::SliderFloat(T(TKEY("stamp_radius"), "Stamp Radius"), &settings.StampRadius, 4.0f, 128.0f, "%.0f");
		if (auto _ttStamp = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("stamp_radius_tooltip"), "Scales the Havok collision-shape radii used for stamping (20 = the shapes' actual size). Stamps come from actors' real collision shapes — feet and legs carve individually."));
		ImGui::Checkbox(T(TKEY("refill_only_snowing"), "Refill Only While Snowing"), &settings.RefillOnlyWhenSnowing);
		if (auto _ttRefillSnow = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("refill_only_snowing_tooltip"), "Compressed snow only recovers while the current weather is snowing. Trails and trenches persist through clear weather."));
		ImGui::SliderFloat(T(TKEY("refill_time"), "Snow Refill Time"), &settings.RefillTime, 0.0f, 3600.0f, "%.0f s");
		if (auto _ttRefill = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("refill_time_tooltip"), "Time for compressed snow to fully recover. 0 disables refilling."));

		if (ImGui::TreeNodeEx(T(TKEY("model_depths"), "Snow Depth by Model Class"), ImGuiTreeNodeFlags_Framed)) {
			if (auto _ttModels = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("model_depths_tooltip"), "Snow layer height per OBJECT model class. Roads are matched by their road/bridge names and textures; flat vs round is classified automatically per mesh."));
			ImGui::SliderFloat(T(TKEY("road_meshes_depth"), "Road Meshes"), &settings.RoadMeshesDepth, 0.0f, 64.0f, "%.0f units");
			if (auto _ttRoad = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("road_meshes_depth_tooltip"), "Snow layer on road and bridge meshes. Kept below the surrounding snow classes so the road's course stays readable through the snowfield."));
			ImGui::SliderFloat(T(TKEY("objects_snow_depth"), "Flat Objects"), &settings.ObjectsSnowDepth, 0.0f, 25.0f, "%.0f units");
			if (auto _ttObj = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("objects_snow_depth_tooltip"), "Snow layer on flat hard-edged meshes (walkways, roofs, planks) — these get a completely flat overlay, no fake 3D. Classified automatically per mesh."));
			ImGui::SliderFloat(T(TKEY("snow_meshes_depth"), "Round Objects"), &settings.SnowMeshesDepth, 0.0f, 25.0f, "%.0f units");
			if (auto _ttMesh = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("snow_meshes_depth_tooltip"), "Snow layer on organically smooth meshes (rocks, drifts, logs), where the puffed pillow layer reads correctly in 3D."));
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx(T(TKEY("class_depths"), "Snow Depth by Texture Class"), ImGuiTreeNodeFlags_Framed)) {
			if (auto _ttClasses = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("class_depths_tooltip"), "Shell height per snow texture family (classified by the vanilla LTEX filenames every retexture mod overrides). Negative values submerge the shell below the surface. Retunes live from cached data."));
			bool classDepthsChanged = false;
			for (uint32_t classI = 0; classI < kSnowClassCount; ++classI)
				classDepthsChanged |= ImGui::SliderFloat(kSnowClasses[classI].label, &settings.SnowClassDepths[classI], -20.0f, 64.0f, "%.0f units");
			if (classDepthsChanged)
				shellDataDirty.store(true, std::memory_order_release);
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx(T(TKEY("snow_borders"), "Snow Borders"), ImGuiTreeNodeFlags_Framed)) {
			if (auto _ttBorders = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("snow_borders_tooltip"), "How the shell behaves where two texture classes with different snow depths meet (deep snow next to mud, roads, coast...)."));
			ImGui::SliderFloat(T(TKEY("border_noise"), "Border Noise"), &settings.SnowBorderNoise, 0.0f, 64.0f, "%.0f units");
			if (auto _ttBn = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("border_noise_tooltip"), "Wiggles WHERE the depth border between neighboring texture classes falls, so snow edges wander organically instead of tracing the texture seam."));
			ImGui::SliderFloat(T(TKEY("border_smoothness"), "Border Smoothness"), &settings.SnowBorderSmoothness, 0.0f, 64.0f, "%.0f units");
			if (auto _ttBs = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("border_smoothness_tooltip"), "Widens the depth ramp between neighboring classes so deep snow meets shallow ground in a slope instead of a ravine wall."));
			ImGui::SliderFloat(T(TKEY("border_trampled_fade"), "Trampled Border Fade"), &settings.SnowBorderTrampledFade, 0.0f, 64.0f, "%.0f units");
			if (auto _ttTf = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("border_trampled_fade_tooltip"), "How gradually TRAMPLED snow (trench floors) blends out toward a class border, letting the ground beneath show through faintly. Too high and the landscape becomes too visible under trenches."));
			ImGui::SliderFloat(T(TKEY("border_untrampled_fade"), "Untrampled Border Fade"), &settings.SnowBorderUntrampledFade, 0.0f, 64.0f, "%.0f units");
			if (auto _ttUf = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("border_untrampled_fade_tooltip"), "How gradually UNTRAMPLED snow dissolves at a class border. Shorter = the pristine snow edge commits sooner."));
			ImGui::SliderFloat(T(TKEY("snow_snow_fade"), "Snow <-> Snow Fade"), &settings.SnowSnowFade, 0.0f, 64.0f, "%.0f units");
			if (auto _ttSs = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("snow_snow_fade_tooltip"), "Cross-fade between OBJECT snow and LANDSCAPE snow where their surfaces run close in height (road meshes, low platforms). Wider = the two snow kinds dither into each other instead of meeting at a hard seam."));
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx(T(TKEY("snow_mounds"), "Snow Mounds"), ImGuiTreeNodeFlags_Framed)) {
			if (auto _ttMounds = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("snow_mounds_tooltip"), "How raised snow settles around whatever lifts it."));
			ImGui::SliderFloat(T(TKEY("mound_steepness"), "Mound Steepness"), &settings.SnowMoundSteepness, 0.5f, 3.0f, "%.1f");
			if (auto _ttSteep = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("mound_steepness_tooltip"), "Angle of repose for snow mounds (1.0 = 45 degrees). Steeper = raised snow clings tighter: narrow banks instead of broad aprons, juttier mounds."));
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx(T(TKEY("render_distance"), "Render Distance"), ImGuiTreeNodeFlags_Framed)) {
			if (auto _ttRd = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("render_distance_tooltip"), "How far each snow system reaches. Higher = more VRAM and GPU cost."));
			ImGui::SliderFloat(T(TKEY("range_shell"), "Snow Shell"), &settings.RangeShellM, 94.0f, 750.0f, "%.0f m");
			if (auto _ttRs = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("range_shell_tooltip"), "Warped-grid span. Applies live; near-field vertex density scales with range (8-unit spacing at 375 m)."));
			ImGui::SliderFloat(T(TKEY("range_trenches"), "Trenches"), &settings.RangeTrenchesM, 29.0f, 200.0f, "%.0f m");
			if (ImGui::IsItemDeactivatedAfterEdit())
				trenchRangeDirty = true;
			if (auto _ttRt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("range_trenches_tooltip"), "Deformation window radius (also the actor stamping cutoff). Applying a change CLEARS existing trenches; trench detail coarsens with range."));
			ImGui::SliderFloat(T(TKEY("range_skins"), "Object Snow"), &settings.RangeSkinsM, 29.0f, 750.0f, "%.0f m");
			if (auto _ttRk = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("range_skins_tooltip"), "Capture radius for snow skins on objects (rocks, cliffs, roofs). Applies live."));
			ImGui::SliderFloat(T(TKEY("range_skins_fade"), "Distant Snow Blend"), &settings.RangeSkinsFadeM, 29.0f, 750.0f, "%.0f m");
			if (auto _ttRkf = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("range_skins_fade_tooltip"), "Distance where object snow starts dissolving back into the object's own appearance; fully faded by the Object Snow range end. Cures distant blank-white objects."));
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx(T(TKEY("debug_options"), "Debugging Options"), ImGuiTreeNodeFlags_Framed)) {
			ImGui::Checkbox(T(TKEY("show_debug"), "Show Deformation Map"), &settings.ShowDebugTexture);
			if (settings.ShowDebugTexture) {
				ImGui::Text("%s", T(TKEY("debug_hint"), "White = compressed snow. The map follows the camera."));
				ImGui::Image(GetDeformationSRV(), { 512.0f, 512.0f });
			}

			if (ImGui::Button(T(TKEY("clear"), "Clear Deformation Map")))
				clearRequested = true;

			ImGui::Checkbox(T(TKEY("shell_data_debug"), "Shell: Data Debug Plane"), &shellDataDebug);
			if (auto _ttPlane = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("shell_data_debug_tooltip"), "Renders the shell as an always-visible conforming plane colored by the terrain data it samples: red = height, green = snow coverage, blue = ramp depth. Black = no data reaches the shader."));

			ImGui::Checkbox(T(TKEY("debug_overlay"), "Debug Terrain Overlay"), &debugTerrainOverlay);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("debug_overlay_tooltip"), "Paints diagnostics on terrain: red = outside deformation window, green = deformation, blue = detected snow."));

			ImGui::TreePop();
		}

		ImGui::Text("Snow statics captured: %u", statCapturedStatics.load(std::memory_order_relaxed));
		ImGui::Text("Snow mask cache: %zu entries, %llu hits, %llu misses",
			snowMasksSizeForUI(),
			(unsigned long long)landMaskHits.load(std::memory_order_relaxed),
			(unsigned long long)landMaskMisses.load(std::memory_order_relaxed));
		ImGui::Text("Terrain data: %zu cells baked, %u in window, %u snow texels, height range [%.0f, %.0f]",
			ShellCellCountForUI(), shellStatCellsInWindow, shellStatSnowTexels,
			shellStatMinHeight, shellStatMaxHeight);
		ImGui::Text("Shadow source: descriptors=%u endSplits=%.0f/%.0f/%.0f atlasSlices=%u",
			dbgLodDescriptorCount, dbgLodEndSplits[0], dbgLodEndSplits[1], dbgLodEndSplits[2], dbgLodAtlasSlices);

		ImGui::TreePop();
	}
}
