#include "Features/SnowDeformation.h"

#include <imgui_stdlib.h>

#include "Features/SnowDeformation/AlphaBuild.h"
#include "Utils/Game.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "feature.snow_deformation."

void SnowDeformation::DrawSettings()
{
	ImGui::TextDisabled("%s", T(TKEY("credit"), "Snow Deformation - by josefplyr1"));
	if (auto _ttCredit = Util::HoverTooltipWrapper())
		ImGui::Text("%s", kAttribution);
	ImGui::Separator();

	ImGui::Checkbox(T(TKEY("enable"), "Enable Snow Deformation"), &settings.EnableSnowDeformation);

	// The seven performance levers in one click (FPS-STABILISATION-PLAN.md
	// §7.2 vectors); nothing else is touched.
	{
		auto applyPreset = [&](uint a_mapDim, float a_trenchesM, float a_skinsM, float a_skinsGeomM, bool a_tess, float a_parallaxDepth, float a_parallaxShadow) {
			if (settings.DeformMapResolution != a_mapDim) {
				settings.DeformMapResolution = a_mapDim;
				deformMapDimDirty = true;
			}
			if (settings.RangeTrenchesM != a_trenchesM) {
				settings.RangeTrenchesM = a_trenchesM;
				trenchRangeDirty = true;
			}
			settings.RangeSkinsM = a_skinsM;
			settings.RangeSkinsGeometryM = a_skinsGeomM;
			settings.Tessellation = a_tess;
			settings.ParallaxDepth = a_parallaxDepth;
			settings.ParallaxShadowStrength = a_parallaxShadow;
		};
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(T(TKEY("quality_presets"), "Quality Preset:"));
		if (auto _ttPresets = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("quality_presets_tooltip"), "Sets the seven performance settings in one click: Deformation Map Resolution, the three Distant Snow ranges, Tessellate Trenches and the two Parallax dials. Everything else keeps its value, and any of the seven can still be tweaked afterwards. Ultra assumes upscaling. Applying can clear existing trenches (a resolution or Trenches-range change does); remembered trenches are re-injected."));
		ImGui::SameLine();
		if (ImGui::Button(T(TKEY("preset_low"), "Low")))
			applyPreset(1024u, 60.0f, 100.0f, 50.0f, false, 0.0f, 0.0f);
		ImGui::SameLine();
		if (ImGui::Button(T(TKEY("preset_medium"), "Medium")))
			applyPreset(2048u, 80.0f, 150.0f, 60.0f, true, 0.33f, 0.15f);
		ImGui::SameLine();
		if (ImGui::Button(T(TKEY("preset_high"), "High")))
			applyPreset(2048u, 100.0f, 250.0f, 80.0f, true, 0.66f, 0.25f);
		ImGui::SameLine();
		if (ImGui::Button(T(TKEY("preset_ultra"), "Ultra")))
			applyPreset(4096u, 125.0f, 750.0f, 100.0f, true, 1.0f, 0.5f);
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

		ImGui::Checkbox(T(TKEY("proj_snow_match"), "Match Projected Snow"), &settings.ProjSnowMatch);
		if (auto _ttPsm = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("proj_snow_match_tooltip"), "The game paints snow onto rocks, roofs and logs by projecting a separate snow texture from above. This swaps that projection's texture and material response for the snow shell's own set, so painted-on snow matches the shell instead of reading as a different snow. Only draws whose projected material really is snow are touched — sand and moss projections keep their look."));

		ImGui::PushID("general_settings");
		if (ImGui::TreeNodeEx(T(TKEY("menu_advanced"), "Advanced"))) {
			ImGui::Checkbox(T(TKEY("sss_remarch"), "Re-march Shadows on the Shell"), &settings.ShellSSSRemarch);
			if (auto _ttRemarch = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("sss_remarch_tooltip"), "Screen-Space Shadows are normally marched on the ground BENEATH the snow, so the shell can only use them at distance or they print buried objects through the snow. This re-marches them from the snow surface and accepts only casters standing above the snow line - which brings back near-field grass and contact shadows, including from actors, without the prints. Costs 8 depth taps per lit shell pixel. A/B this against it being off."));

			ImGui::Checkbox(T(TKEY("sss_remarch_thickness"), "Streak Fix (Occluder Thickness)"), &settings.ShellSSSRemarchThickness);
			if (auto _ttThick = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("sss_remarch_thickness_tooltip"), "The Bend SSS thin-shell rule applied to the re-march: an occluder only shadows ray samples within 48 units of itself, instead of everything behind it on screen - which is what painted a character's silhouette as a long streak across the snow. Grass and other thin casters are unaffected; the trade is slightly lighter shadow directly behind very thick objects. Only does anything with the re-march on."));

			ImGui::SliderFloat(T(TKEY("sss_remarch_cap"), "Caster Height Cap"), &settings.ShellSSSRemarchCasterCap, 10.0f, 200.0f, "%.0f units");
			if (auto _ttCap = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("sss_remarch_cap_tooltip"), "The re-march only accepts casters SHORTER than this above the snow line. Anything taller - people, fences, trees - already casts real shadows via the cascades, so its re-marched copy is the doubled soft bleed around actors. 20 = short grass only (default); 200 = accept everything. Only does anything with the re-march on."));

			ImGui::Checkbox(T(TKEY("shell_bare_ground_cull"), "Bare-Ground Cull"), &settings.ShellBareGroundCull);
			if (auto _ttBare = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("shell_bare_ground_cull_tooltip"), "Stops drawing the snow layer over ground that has no snow class under it at all - sand, riverbed, road, seafloor. There the layer sits below the terrain and cannot produce a pixel, so skipping it is free. The test is the -8 floor, the depth ground reaches only when every texture under it is a non-snow class, so the ramp that climbs into a snow layer is never cut and edges are unaffected. Leave on."));

			ImGui::TreePop();
		}
		ImGui::PopID();

		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("distant_snow"), "Distant Snow"), ImGuiTreeNodeFlags_Framed)) {
		if (auto _ttDs = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("distant_snow_tooltip"), "Snow on far terrain the game hasn't loaded: heights come from the worldspace heightmap (shipped with Community Shaders), and snow placement follows the game's own distant LOD textures — where the LOD is painted snowy, our snow appears. Loaded terrain always uses its real snow textures instead. The sliders here also set how far each snow system reaches; higher = more VRAM and GPU cost."));
		bool distantChanged = false;

		ImGui::Checkbox(T(TKEY("horizon_snow"), "Horizon Snow"), &settings.HorizonSnow);
		if (auto _ttHs = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("horizon_snow_tooltip"), "Recolors the game's distant LOD terrain with the shell's own snow material wherever its bake reads as snow, so snow appearance stays consistent from your feet to the horizon. The snow shell ends at the loaded-cell boundary and this takes over from there, out to the edge of the world."));

		ImGui::Checkbox(T(TKEY("glacier_snow_match"), "Match Glacier Snow"), &settings.GlacierSnowMatch);
		if (auto _ttGsm = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("glacier_snow_match_tooltip"), "Glaciers and icebergs carry snow baked into their meshes, which never matches the shell's material. This recolors their up-facing snow to the shell's own set at every distance, and stops the object snow shell from trying to wrap these huge meshes — its conforming window can't cover them, which produced square patches, doubled layers and rim gaps."));

		distantChanged |= ImGui::SliderFloat(T(TKEY("lod_snow_sensitivity"), "LOD Snow Detection"), &settings.LODSnowSensitivity, 0.0f, 1.0f, "%.2f");
		if (auto _ttLss = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("lod_snow_sensitivity_tooltip"), "How eagerly a distant LOD texture pixel counts as snow. The scale was widened: the old best-at-1.0 now sits near 0.5. Low = only bright white; high = pale gray rock starts counting too. The same setting drives the Horizon Snow recolor, so it decides where snow sits on the far terrain as well as how the shell reads it."));

		ImGui::SliderFloat(T(TKEY("range_trenches"), "Trenches"), &settings.RangeTrenchesM, 29.0f, 200.0f, "%.0f m");
		if (ImGui::IsItemDeactivatedAfterEdit())
			trenchRangeDirty = true;
		if (auto _ttRt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("range_trenches_tooltip"), "Deformation window radius (also the actor stamping cutoff). Applying a change CLEARS existing trenches. NOT a performance setting: cost follows Deformation Map Resolution, not range. Texel detail is range divided by resolution, so a smaller range at the same resolution means sharper footprints over a shorter reach."));

		ImGui::SliderFloat(T(TKEY("range_skins"), "Object Snow"), &settings.RangeSkinsM, 29.0f, 750.0f, "%.0f m");
		if (auto _ttRk = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("range_skins_tooltip"), "Capture radius for snow skins on objects (rocks, cliffs, roofs); skins dissolve softly over the last ~29 m of it. Loaded cells bound the real reach (~200 m at uGridsToLoad 5), so values above that only matter with a larger uGridsToLoad. Applies live."));

		ImGui::SliderFloat(T(TKEY("range_skins_geometry"), "Object Snow Geometry Range"), &settings.RangeSkinsGeometryM, 10.0f, 200.0f, "%.0f m");
		if (auto _ttRkg = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("range_skins_geometry_tooltip"), "Distance where raised snow on objects flattens back into a painted layer. The layer's height sinks to zero before the skins' own distance dissolve starts, so the switch has no silhouette to pop. Deep snow classes keep their height further out than thin ones. Higher values keep real snow depth further out at the cost of more geometry work."));

		ImGui::SliderFloat(T(TKEY("skin_distant_bareness"), "Distant Bare Rock"), &settings.SkinDistantBareness, 0.0f, 1.0f, "%.2f");
		if (auto _ttSdb = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("skin_distant_bareness_tooltip"), "How much bare rock distant cliffs and boulders keep. Close up, snow coverage follows the smoothed mesh normal, which on low-poly rocks reports steep flanks as up-facing; near the camera the edge taper hides that, but at range it turns a rock into a white blob. This hands the coverage test over to each face's true orientation as the object shrinks, so steep faces shed their snow again. Raise it for more exposed rock; too high and the mesh's own triangles start to read as jagged facets and seams. 0 keeps the old behaviour."));

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
			ImGui::Text("%s", T(TKEY("accumulation_melt_hours_tooltip"), "How long clear weather takes to settle the layer back down from the peak. Deliberately longer than the build-up: snow that took a day to fall should not be gone by lunchtime, and the imbalance is what lets a snowy stretch stay deep between storms."));

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
			ImGui::Text("%s", T(TKEY("parallax_depth_tooltip"), "Parallax occlusion mapping on the landscape shell: marches the view ray through the snow texture's displacement map and shades from where it hits, so grain occludes grain and the surface reads as thick instead of merely lit. It moves no vertices, and the depth it resolves is by construction the depth of the grain being drawn. A multiplier on the PBR config's displacementScale - 1.0 is exactly the slab depth PBR ground gets. 0 skips the march."));

		ImGui::SliderFloat(T(TKEY("parallax_shadow_strength"), "Parallax Shadow"), &settings.ParallaxShadowStrength, 0.0f, 2.0f, "%.2fx");
		if (auto _ttPss = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("parallax_shadow_strength_tooltip"), "Self-shadowing of the snow's own grain, the same term PBR ground receives from Extended Materials: four taps along the sun through the displacement map, so the micro-relief casts into itself under low sun instead of reading flat. Needs the PBR snow set's _p map. 0 skips the taps entirely (and is the A/B for their cost)."));

		ImGui::TreePop();
	}

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
			ImGui::Text("%s", T(TKEY("class_depths_tooltip"), "Starting height for each snow texture family (classified by the vanilla LTEX filenames every retexture mod overrides). This is the DEFAULT a texture uses until it is given its own value below. Negative values submerge the shell below the surface. Retunes live from cached data."));
		bool classDepthsChanged = false;
		for (uint32_t classI = 0; classI < kSnowClassCount; ++classI)
			classDepthsChanged |= ImGui::SliderFloat(kSnowClasses[classI].label, &settings.SnowClassDepths[classI], -20.0f, 64.0f, "%.0f units");
		if (classDepthsChanged) {
			RefreshLandTextureDepths();
			shellDataDirty.store(true, std::memory_order_release);
		}
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

		ImGui::Text("%zu textures loaded this session", textures.size());

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

	if (ImGui::TreeNodeEx(T(TKEY("snow_borders"), "Snow Borders"), ImGuiTreeNodeFlags_Framed)) {
		if (auto _ttBorders = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("snow_borders_tooltip"), "How the shell behaves where two texture classes with different snow depths meet (deep snow next to mud, roads, coast...)."));
		ImGui::Checkbox(T(TKEY("border_dithering"), "Border Dithering"), &settings.SnowBorderDithering);
		if (auto _ttBd = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("border_dithering_tooltip"), "Scatters a thin dusting of snow just beyond the committed edge onto the ground, like windblown spill. Off = a clean binary cut. Border Fade controls how far the dust reaches."));

		ImGui::SliderFloat(T(TKEY("workspace_clearing_size"), "Workspace Clearing Size"), &settings.TrampleZoneScale, 0.25f, 2.0f, "%.2fx");
		if (auto _ttWcs = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("workspace_clearing_size_tooltip"), "Radius multiplier for the snow bowls around workstations, smelters, forges, stalls, wells and shrines. Applies within a second."));

		ImGui::SliderFloat(T(TKEY("workspace_clearing_height"), "Workspace Clearing Height"), &settings.TrampleZoneHeight, 0.0f, 100.0f, "%.0f%%");
		if (auto _ttWch = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("workspace_clearing_height_tooltip"), "Snow height remaining in a workspace bowl, as a percent of the surrounding depth. 0 melts to the floor, 100 disables the clearing. Applies within a second."));

		ImGui::PushID("snow_borders");
		if (ImGui::TreeNodeEx(T(TKEY("menu_advanced"), "Advanced"))) {
			ImGui::SliderFloat(T(TKEY("border_noise"), "Border Noise"), &settings.SnowBorderNoise, 0.0f, 64.0f, "%.0f units");
			if (auto _ttBn = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("border_noise_tooltip"), "Wiggles WHERE the depth border between neighboring texture classes falls, so snow edges wander organically instead of tracing the texture seam."));

			ImGui::SliderFloat(T(TKEY("border_smoothness"), "Border Smoothness"), &settings.SnowBorderSmoothness, 0.0f, 64.0f, "%.0f units");
			if (auto _ttBs = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("border_smoothness_tooltip"), "Widens the depth ramp between neighboring classes so deep snow meets shallow ground in a slope instead of a ravine wall."));

			ImGui::SliderFloat(T(TKEY("border_fade"), "Border Fade"), &settings.SnowBorderFade, 0.0f, 100.0f, "%.0f%%");
			if (auto _ttBf = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("border_fade_tooltip"), "How much of the snow's edge takes part in the height contest against the ground - higher values make the scattered dust past the edge broader and more visible. Border Dithering must be on for the dust itself."));

			ImGui::TreePop();
		}
		ImGui::PopID();

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
				ImGui::Text("%s", T(TKEY("map_resolution_tooltip"), "Resolution of the map every trench, melt mark and berm lives in. The main performance dial for this section: cost scales with the square of it, so each step down roughly quarters the trench passes' GPU time. Detail follows the Trenches range too - at the default range, 2048 gives roughly 12 cm per texel, 1024 roughly 24 cm (footprints soften but trails stay). Changing it clears the map; remembered trenches are re-injected from the store."));
		}

		ImGui::Checkbox(T(TKEY("tessellation"), "Tessellate Trenches"), &settings.Tessellation);
		if (auto _ttTess = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("tessellation_tooltip"), "Adds vertex density to the shell and the object trench patch near the camera, keyed off the deformation map, so carves resolve as smooth walls instead of following the coarse grid. This is what trench smoothness actually depends on. Off costs nothing but leaves every trench as angular as the grid beneath it."));

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
			ImGui::Text("%s", T(TKEY("trench_detail_group_tooltip"), "The shape and surface of disturbed snow: the raised berm along trench edges, the chunky churned surface and the broken rim. Snow sitting on objects follows these too."));

		ImGui::SliderFloat(T(TKEY("berm_height"), "Berm Height"), &settings.BermHeight, 0.0f, 1.0f, "%.2fx");
		if (auto _ttBh = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("berm_height_tooltip"), "Height of the pushed-aside snow ridge along trench edges, as a fraction of the local snow depth. 0 removes the berm. On objects the same value shades a ridge rather than raising one."));

		ImGui::SliderFloat(T(TKEY("berm_clods"), "Berm Clods"), &settings.BermClods, 0.0f, 6.0f, "%.1f units");
		if (auto _ttClods = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("berm_clods_tooltip"), "Breaks the berm crest into coarse thrown chunks - spoil is clumps, not a smooth mound. Coarser than the trench's churn rubble on purpose, so berm and trench read at different scales. 0 = off."));

		ImGui::SliderFloat(T(TKEY("churn_height"), "Churn Height"), &settings.ChurnHeight, 0.0f, 8.0f, "%.1f units");
		if (auto _ttCh = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("churn_height_tooltip"), "How tall the broken snow lumps are in trenches and on berms. 0 leaves disturbed snow smooth."));

		ImGui::SliderFloat(T(TKEY("churn_size"), "Churn Size"), &settings.ChurnSize, 0.25f, 4.0f, "%.2fx");
		if (auto _ttCs = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("churn_size_tooltip"), "Size of the broken snow lumps: smaller = finer rubble, larger = broad clods."));

		ImGui::SliderFloat(T(TKEY("rim_lip"), "Rim Lip"), &settings.RimLip, 0.0f, 0.3f, "%.2f");
		if (auto _ttLip = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("rim_lip_tooltip"), "The trench rim rolls UP slightly before it drops - the cornice look of cut snow. Height as a fraction of local snow depth; deep snow only (shallow dimples stay smooth). 0 = off."));

		ImGui::SliderFloat(T(TKEY("rim_teeth"), "Rim Teeth"), &settings.RimTeeth, 0.0f, 1.0f, "%.2f");
		if (auto _ttTeeth = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("rim_teeth_tooltip"), "Breaks the trench edge into irregular teeth and blocks instead of a clean curve, using the border system's noise. Deep snow only. Too high eats the trench's readable width - back off if trails start looking chewed. 0 = off."));

		ImGui::SeparatorText(T(TKEY("bow_wave_group"), "Bow Wave"));

		ImGui::SliderFloat(T(TKEY("bow_wave_height"), "Bow Wave Height"), &settings.BowWaveHeight, 0.0f, 1.5f, "%.2f");
		if (auto _ttBwH = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("bow_wave_height_tooltip"), "How high a moving body heaps the snow it is pushing, as a fraction of local snow depth. This is the crest that rides ahead of and beside the legs and relaxes into the berm behind. 0 = off."));

		ImGui::SliderFloat(T(TKEY("bow_wave_reach"), "Bow Wave Reach"), &settings.BowWaveReach, 0.25f, 3.0f, "%.2fx");
		if (auto _ttBwR = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("bow_wave_reach_tooltip"), "How far AHEAD of the feet the pushed snow piles, and how far the hill stretches along the direction of travel. It does not make the mound bigger - it moves it out in front and draws it out longer, which is what a body ploughing a furrow leaves. Width is set by Forward Bias."));

		ImGui::SliderFloat(T(TKEY("bow_wave_forward"), "Bow Wave Forward Bias"), &settings.BowWaveForward, 0.0f, 1.0f, "%.2f");
		if (auto _ttBwF = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("bow_wave_forward_tooltip"), "0 = snow heaps evenly all around the actor; 1 = only dead ahead. Middle values give the crescent - pushed mostly forward but shouldered aside too."));

		ImGui::SliderFloat(T(TKEY("bow_wave_chunk"), "Bow Wave Chunkiness"), &settings.BowWaveChunk, 0.0f, 1.0f, "%.2f");
		if (auto _ttBwC = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("bow_wave_chunk_tooltip"), "How far the pushed snow breaks into uneven lumps instead of a smooth swell. 0 reads as a water wave; higher gives chunks that ride up and tumble aside. The lumps are anchored to the world, so they appear to flow through the crest as you advance rather than travelling with you."));

		ImGui::SliderFloat(T(TKEY("bow_wave_speed"), "Bow Wave Full Speed"), &settings.BowWaveFullSpeed, 40.0f, 500.0f, "%.0f u/s");
		if (auto _ttBwS = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("bow_wave_speed_tooltip"), "Travel speed at which the crest reaches full height. Lower means a walk already pushes a wave; higher means only a sprint does. The crest builds quickly and eases out over about a third of a second when you stop."));

		ImGui::PushID("snow_trenches");
		if (ImGui::TreeNodeEx(T(TKEY("menu_advanced"), "Advanced"))) {
			ImGui::SliderFloat(T(TKEY("compact_matte"), "Compaction Matte"), &settings.CompactMatte, 0.0f, 1.0f, "%.2f");
			if (auto _ttCm = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("compact_matte_tooltip"), "How completely trampled snow loses its sparkle. Packing crushes the loose crystals that glint, so trench floors, walls and berms go matte while untouched snow keeps full glitter. Both shells; 0 = off."));

			ImGui::SliderFloat(T(TKEY("stamp_radius"), "Stamp Radius"), &settings.StampRadius, 4.0f, 128.0f, "%.0f");
			if (auto _ttStamp = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("stamp_radius_tooltip"), "Scales the Havok collision-shape radii used for stamping (20 = the shapes' actual size). Stamps come from actors' real collision shapes — feet and legs carve individually."));

			ImGui::SliderFloat(T(TKEY("footprint_width"), "Footprint Width"), &settings.FootPrintScale, 0.5f, 3.0f, "%.2f x");
			if (auto _ttFw = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("footprint_width_tooltip"), "Width multiplier on foot prints; length follows the skeleton. Snow collapses wider than the foot, so above 1.0 usually reads best."));

			ImGui::SliderFloat(T(TKEY("snow_slumping"), "Snow Slumping"), &settings.SlumpRate, 0.0f, 1.0f, "%.2f");
			if (auto _ttSlump = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("snow_slumping_tooltip"), "Snow dug away on BOTH sides loses its support and settles about halfway into a low uneven bump, so heavy traffic reads as one churned channel instead of a comb of full-height fins. Trench walls and open snow never move. 0 = off; higher = faster settling."));

			ImGui::SliderFloat(T(TKEY("trench_sharpness"), "Trench Wall Sharpness"), &settings.TrenchWallSharpness, 0.0f, 100.0f, "%.0f %%");
			if (auto _ttSharp = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("trench_sharpness_tooltip"), "How steeply trench walls drop. Low = wide, soft banks; 100 = full depth held to the trail's very edge."));

			ImGui::SliderFloat(T(TKEY("trench_floor_height"), "Trench Floor Height"), &settings.TrenchFloorHeight, 0.0f, 8.0f, "%.1f units");
			if (auto _ttTfh = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("trench_floor_height_tooltip"), "Minimum snow left on carved trench floors, in units above the terrain. Low values let deep trampling wear through to the real ground, like snow does; 5 restores the old always-solid floors."));

			ImGui::SliderFloat(T(TKEY("mound_steepness"), "Mound Steepness"), &settings.SnowMoundSteepness, 0.5f, 3.0f, "%.1f");
			if (auto _ttSteep = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("mound_steepness_tooltip"), "Angle of repose for snow mounds (1.0 = 45 degrees). Steeper = raised snow clings tighter: narrow banks instead of broad aprons, juttier mounds."));

			ImGui::Checkbox(T(TKEY("no_carve_floating"), "Floating Actors Leave No Trench"), &settings.NoCarveFloatingActors);
			if (auto _ttFloat = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("no_carve_floating_tooltip"), "Stops things that never touch the ground from digging it: atronachs, wisps, ghosts, anything that hovers. Nothing is named - an actor is judged by whether its own lowest part ever comes down to its footing, so modded levitators are covered too."));

			ImGui::SliderFloat(T(TKEY("floating_band"), "Floating Actor Clearance"), &settings.FloatingActorBand, 4.0f, 80.0f, "%.0f units");
			if (auto _ttFloatBand = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("floating_band_tooltip"), "How far an actor's lowest part may sit above its footing and still count as standing on it. Lower values catch things that only just hover, at the risk of dropping a normal creature's tracks mid-stride."));

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

			ImGui::SeparatorText(T(TKEY("menu_experimental"), "Experimental"));

			ImGui::Checkbox(T(TKEY("object_trenches"), "Trenches on Objects"), &settings.ObjectTrenches);
			if (auto _ttOt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("object_trenches_tooltip"), "Carve footprints into snow sitting on objects (rocks, logs, roofs). Off while the object trenching is being reworked; roads and bridges keep their trenches either way."));

			ImGui::Checkbox(T(TKEY("road_heightfield"), "Road Snow As One Surface"), &settings.RoadHeightfield);
			if (auto _ttRhf = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("road_heightfield_tooltip"), "Experimental. Road snow becomes a single deformable surface that dips underfoot, instead of a flat sheet with a separate trench carved beneath it. Nearby roads only for now, and bridges are left on the old path."));

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
			ImGui::Text("%s", T(TKEY("spell_cat_force_tooltip"), "Force is the one school that moves snow without changing it. Everything else removes it, burns it or freezes it; a shove pushes it aside, so it is also the only one that piles a ridge at the far lip - and that ridge is what makes Unrelenting Force read as pushed rather than deleted."));
		ImGui::Checkbox(T(TKEY("shout_cones"), "Shouts Plough The Snow"), &settings.EnableShoutCones);
		if (auto _ttShout = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("shout_cones_tooltip"), "Lets a shout work the whole wedge of ground in front of the shouter rather than only the spot its projectile happens to strike. Each shout takes its reach from its own record, so a dragon's breath carries ten times a Greybeard's without anything being named. Off, shouts still mark through whatever they throw."));

		ImGui::SliderFloat(T(TKEY("shout_spread"), "Shout Cone Spread"), &settings.ShoutConeSpread, 5.0f, 150.0f, "%.0f deg");
		if (auto _ttSpread = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("shout_spread_tooltip"), "How wide the wedge opens. The records author a shout's REACH but never its width, so this is the one thing about a shout's shape that has to be taste rather than a reading."));

		ImGui::SliderFloat(T(TKEY("force_carve_depth"), "Force Carve Depth"), &settings.ForceCarveDepth, 0.0f, 1.0f, "%.2f");
		if (auto _ttForceDepth = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("force_carve_depth_tooltip"), "How deep a full-strength shove ploughs, as a fraction of the layer. Each shout scales down from here by the impact force its own record carries, so a dragon's shout bites harder than a breath's shove without either being named."));

		ImGui::PushID("spell_cat_force");
		if (ImGui::TreeNodeEx(T(TKEY("menu_advanced"), "Advanced"))) {
			ImGui::SliderFloat(T(TKEY("shout_length"), "Shout Cone Reach"), &settings.ShoutConeLength, 0.1f, 3.0f, "%.2fx");
			if (auto _ttLen = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("shout_length_tooltip"), "Multiplier on the reach each shout's own projectile authors. At 1.0 you get exactly what the game says - about 1000 units for Unrelenting Force, 1200 for the breaths, and far more for a dragon. Aiming upward shortens it; aiming at the sky throws it away, since the snow layer cannot hold a mark in the air."));

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
			ImGui::Text("%s", T(TKEY("blast_radius_scale_tooltip"), "Size of the crater a detonation leaves, against the radius the explosion itself authors. Those radii are tuned for how far the blast HURTS, which is a good deal wider than the ground it should scar."));

		ImGui::PushID("spell_cat_fire");
		if (ImGui::TreeNodeEx(T(TKEY("menu_advanced"), "Advanced"))) {
			ImGui::SliderFloat(T(TKEY("melt_persistence"), "Fire Melt Persistence"), &settings.MeltPersistence, 0.0f, 1.0f, "%.2f");
			if (auto _ttPersist = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("melt_persistence_tooltip"), "How much longer melted ground stays bare than trampled ground. The ground under a fire is warm and wet after the flame is gone, so a melt basin outlasts a footprint of the same depth. 0 = both recover at the same rate."));

			ImGui::SliderFloat(T(TKEY("melt_bowl_floor"), "Fire Melt Bowl Floor"), &settings.MeltBowlFloor, 0.0f, 0.9f, "%.2f");
			if (auto _ttBowl = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("melt_bowl_floor_tooltip"), "Shape of a melted hollow. 0 curves from the centre like a bowl, which is how heat actually spreads; higher values hold a flat floor and stand the sides up into walls, which reads as blasted rather than melted."));

			ImGui::SliderFloat(T(TKEY("melt_edge_irregularity"), "Fire Melt Edge Irregularity"), &settings.MeltEdgeIrregularity, 0.0f, 0.6f, "%.2f");
			if (auto _ttMeltEdge = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("melt_edge_irregularity_tooltip"), "How far a melted rim wanders off a perfect circle. This moves the outline only and leaves the surface smooth - a melt basin has a wandering edge but no jagged shards."));

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
			ImGui::Text("%s", T(TKEY("pit_depth_tooltip"), "How deep a discharge pocks the snow, as a fraction of the layer. Lightning scatters snow rather than boring into it, so this sits below a footprint - and unlike a melt it does not deepen with time, however long a cloak crackles over one spot."));

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

			ImGui::Checkbox(T(TKEY("arc_use_texture"), "Use Arc Texture"), &settings.LightningArcUseTexture);
			if (auto _ttArcU = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("arc_use_texture_tooltip"), "Off, the bolt is drawn as a pure light channel - a hot core with a soft falloff. On, the texture below shapes that channel; it modulates rather than replaces, so the bolt stays continuous where the art is blank."));

			ImGui::InputText(T(TKEY("arc_texture_path"), "Arc Texture"), &settings.LightningArcTexturePath);
			if (auto _ttArcT = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("arc_texture_path_tooltip"), "Optional DDS path relative to Data. EMPTY by default and deliberately so: the bolt is drawn as a real core-and-falloff channel that stands on its own, and a guessed vanilla path that resolves to nothing would leave a dark band hanging in the air. Supply one to shape the channel; it modulates rather than replaces."));

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

		ImGui::SeparatorText(T(TKEY("spell_cat_frost"), "Frost"));
		if (auto _ttFrost = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("spell_cat_frost_tooltip"), "Frost neither removes snow nor throws it aside - it refreezes what is there. Crusted snow bears weight, so tracks across it barely print, and it shades as ice rather than powder. Something heavy enough still breaks through."));
		ImGui::Checkbox(T(TKEY("lift_frost"), "Raise Buried Frost Effects"), &settings.LiftFrostEffects);
		if (auto _ttLift = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("lift_frost_tooltip"), "Lifts a wall of frost, and the small ice effects like it, up onto the snow standing over them instead of leaving them buried underneath it. Frost is the only school this happens to: fire melts its own hole and lightning pits one, so those sit in snow they have already taken away, while frost only hardens what is there and leaves the full layer on top of itself. This is the one thing in the whole feature that MOVES something belonging to the game, so it has its own switch."));

		ImGui::SliderFloat(T(TKEY("crust_rate"), "Frost Crust Rate"), &settings.CrustRate, 0.0f, 3.0f, "%.2f /s");
		if (auto _ttCrustRate = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("crust_rate_tooltip"), "How fast frost sets a crust, for a spell of Frostbite's strength. Like a melt this changes how quickly the glaze arrives, never how hard it ends up."));

		ImGui::SliderFloat(T(TKEY("crust_radius"), "Frost Crust Radius"), &settings.CrustRadius, 20.0f, 300.0f, "%.0f");
		if (auto _ttCrustRadius = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("crust_radius_tooltip"), "Reach of a frost source that states none of its own - a cloak, or a held stream. A spell that authors its own area, like Blizzard, uses that instead and ignores this."));

		ImGui::InputText(T(TKEY("frost_texture_path"), "Frost Texture"), &settings.FrostTexturePath);
		if (auto _ttFrostTex = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("frost_texture_path_tooltip"), "DDS path (relative to Data) the frost pattern is drawn from, with its _n companion beside it carrying the crystal structure. It must be a TILEABLE surface - a landscape or cave ice texture - rather than a decal: a decal has its content in the middle and nothing at the edges because it is meant to be printed once, so scattering copies of one only ever gives clumps with gaps between them. Press Reload after changing it."));
		ImGui::SameLine();
		if (ImGui::Button(T(TKEY("reload_frost_texture"), "Reload"))) {
			frostPatternNormalSRV = nullptr;
			frostPatternDiffuseSRV = nullptr;
			frostPatternAttempted = false;
		}

		ImGui::SliderFloat(T(TKEY("frost_pattern"), "Frost Crystal Detail"), &settings.FrostPatternStrength, 0.0f, 2.0f, "%.2f");
		if (auto _ttFrostPat = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("frost_pattern_tooltip"), "Rime crystal picked out on crusted snow, using the game's own frost impact art so it matches the spells landing on it. This is the surface STRUCTURE only - the polish, colour and sheen that make crusted snow read as ice are separate knobs and are untouched. It is sampled without tiling, so a sheet laid by Blizzard or a breath will not show a grid across it however wide it gets."));

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
				ImGui::Text("%s", T(TKEY("crust_break_radius_tooltip"), "Size a shape must reach before it breaks through a crust rather than printing on it, standing in for weight. Low values let anything shatter the glaze; high values let a mammoth walk on it."));

			ImGui::SliderFloat(T(TKEY("frost_pattern_scale"), "Frost Crystal Size"), &settings.FrostPatternScale, 16.0f, 512.0f, "%.0f");
			if (auto _ttFrostScale = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("frost_pattern_scale_tooltip"), "How large the crystal pattern is on the ground. Not a repeat distance - the sampling scatters the texture so there is no repeat to find - just how big the frost structure reads."));

			ImGui::SliderFloat(T(TKEY("crust_gloss"), "Frost Ice Look"), &settings.CrustGloss, 0.0f, 1.0f, "%.2f");
			if (auto _ttCrustGloss = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("crust_gloss_tooltip"), "How strongly crusted snow reads as ice. 0 leaves it looking like ordinary snow that happens to resist footprints."));

			ImGui::SliderFloat(T(TKEY("crust_normal_flatten"), "Frost Ice Smoothness"), &settings.CrustNormalFlatten, 0.0f, 1.0f, "%.2f");
			if (auto _ttCrustFlat = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("crust_normal_flatten_tooltip"), "How far a crust flattens the snow's own surface grain. This is the strongest of the ice cues by a wide margin - powder reads as grain and ice reads as a sheet, so smoothing the surface says frozen over far louder than any change to shine or colour. Turn this down first if the ice reads too strongly."));

			ImGui::SliderFloat(T(TKEY("crust_roughness"), "Frost Ice Roughness"), &settings.CrustRoughness, 0.02f, 0.60f, "%.2f");
			if (auto _ttCrustRough = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("crust_roughness_tooltip"), "Surface roughness of fully crusted snow. Loose snow sits near 0.6; lower values tighten the highlight into a glassy sheet. Needs a light source at a grazing angle to show, so judge it in sunlight rather than under cloud."));

			ImGui::SliderFloat(T(TKEY("crust_sheen"), "Frost Ice Sheen"), &settings.CrustSheen, 0.0f, 3.0f, "%.2f");
			if (auto _ttCrustSheen = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("crust_sheen_tooltip"), "Brightness of the glancing-angle sheen, where a frozen sheet catches the sky and powder does not. Snow is already almost white, so ordinary shine has nowhere left to go - this is the strongest ice cue after smoothness, and the one to reach for if the glaze still reads flat."));

			ImGui::SliderFloat(T(TKEY("crust_specular"), "Frost Ice Shine"), &settings.CrustSpecular, 0.0f, 0.50f, "%.3f");
			if (auto _ttCrustSpec = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("crust_specular_tooltip"), "How strongly crusted snow reflects. Loose snow sits near 0.028, which is so low that a physically honest ice value is invisible next to it - this is a look knob rather than a measurement, so push it until the glaze reads."));

			ImGui::ColorEdit3(T(TKEY("crust_tint"), "Frost Ice Tint"), settings.CrustTint.data());
			if (auto _ttCrustTint = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("crust_tint_tooltip"), "Colour multiplied onto crusted snow. Slightly dark and slightly blue reads as refrozen; pure white leaves the colour alone entirely and lets the smoothness and shine carry it."));

			ImGui::SliderFloat(T(TKEY("atronach_frost_reach"), "Frost Atronach Reach"), &settings.AtronachFrostReach, 0.25f, 4.0f, "%.2fx");
			if (auto _ttAtroFrost = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("atronach_frost_reach_tooltip"), "How much wider a frost atronach freezes than a cast frost source. Unlike the fire one it is heavy and walks, so it digs a trench AND glazes it over - the one thing in the whole feature that leaves a frozen groove, since fire removes snow and force only pushes it aside."));

			ImGui::SliderFloat(T(TKEY("atronach_frost_death"), "Frost Atronach Death Glaze"), &settings.AtronachFrostDeathRadius, 0.0f, 1200.0f, "%.0f");
			if (auto _ttAtroFrostDeath = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("atronach_frost_death_tooltip"), "Radius a frost atronach freezes over when it shatters, before the Blast Radius setting scales it. Deliberately far tighter than the fire one: it breaks apart where it stands rather than detonating outward."));

			ImGui::TreePop();
		}
		ImGui::PopID();

		ImGui::TreePop();
	}

#if !SNOW_ALPHA_BUILD
	if (ImGui::TreeNodeEx(T(TKEY("debug_options"), "Debugging Options"), ImGuiTreeNodeFlags_Framed)) {
		ImGui::SeparatorText(T(TKEY("debug_cat_deform_map"), "Deformation Map"));

		ImGui::Checkbox(T(TKEY("show_debug"), "Show Deformation Map"), &settings.ShowDebugTexture);
		if (settings.ShowDebugTexture) {
			// The honest caption. ImGui blends by the texture's alpha, and this
			// map's .w was claimed by the bow wave's deposit field, so the view
			// is drawn through deposit: transparent wherever nothing has been
			// pushed, whatever the depth channel holds. Right after a load it is
			// blank BY CONSTRUCTION, because deposit is not stored. Two rounds
			// were read backwards from this image before anyone noticed.
			// The map's own depth, copied to a single channel so ImGui cannot
			// draw it through the bow wave's deposit alpha. This is the image
			// that answers "did the trench reach the map".
			ImGui::Text("%s", T(TKEY("debug_hint"), "Deformation map: red = compressed snow. The window is centred on the camera, so the cross is you and the ring is 25 m."));
			const ImVec2 imageTopLeft = ImGui::GetCursorScreenPos();
			if (trenchDebugSRV)
				ImGui::Image(trenchDebugSRV.get(), { 512.0f, 512.0f });
			else
				ImGui::Image(GetDeformationSRV(), { 512.0f, 512.0f });

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
				ImGui::Text("%s", T(TKEY("debug_inject_hint"), "Trench store: what the inject last painted into the window. Opaque, so what you see is what the store holds. Refreshed when the window scrolls or is rebuilt - after a load it is the whole restored window."));
				ImGui::Image(trenchInjectSRV.get(), { 512.0f, 512.0f });
			}
		}

		if (ImGui::Button(T(TKEY("clear"), "Clear Deformation Map"))) {
			clearRequested = true;
			// Deliberate wipe, so the store goes with it: left alone, the
			// inject would put every trench back on the very next frame.
			ClearTrenchStore("the Clear Deformation Map button");
		}

		// S1: the idle skip and its measurement override. The readout names
		// what is holding the pass on, so "why is it running" answers itself.
		ImGui::Checkbox(T(TKEY("debug_force_update"), "Force Deformation Update"), &debugForceDeformationUpdate);
		// S3: the tile dispatch's one-click cross-check. A symptom that
		// vanishes with this on means a dirty-tracking path missed a writer.
		ImGui::Checkbox(T(TKEY("debug_force_tiles"), "Force All Tiles Dirty"), &debugForceAllTilesDirty);
		// The count is the magnitude behind a map-active blocker: a handful of
		// texels is a precision tail, millions is a logic bug. From the newest
		// verdict, so it lags the dispatch by the readback ring.
		ImGui::Checkbox(T(TKEY("debug_activity_view"), "Show Update Activity"), &debugActivityView);
		if (debugActivityView) {
			ImGui::Text("Texels the last pass changed at stored precision. R = depth, G = melt/scorch, B = crust or deposit. Black = the pass rewrote the map byte-identically. The yellow box bounds the changed texels - a few hundred are sub-pixel here without it.");
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
			ImGui::Text("Changed texels (last verdict): %u (depth %u, melt/scorch %u, crust/deposit %u), mean delta %.5f",
				deformChangedTexels, deformChangedDepth, deformChangedMelt, deformChangedCrustDep,
				deformChangedTexels > 0 ? (double)deformChangedDeltaSum * 1e-6 / (double)deformChangedTexels : 0.0);
			if (bboxValid) {
				const float texel = deformWorldSize / std::max((float)deformMapDim, 1.0f);
				const float cx = ((float)(deformChangedMinX + deformChangedMaxX) * 0.5f + 0.5f) * texel;
				const float cy = ((float)(deformChangedMinY + deformChangedMaxY) * 0.5f + 0.5f) * texel;
				const float half = deformWorldSize * 0.5f;
				ImGui::Text("Bbox: (%u,%u)-(%u,%u), %.1f x %.1f m, centre %.1f m E / %.1f m N of camera",
					deformChangedMinX, deformChangedMinY, deformChangedMaxX, deformChangedMaxY,
					(float)(deformChangedMaxX - deformChangedMinX + 1) * texel / kUnitsPerMeter,
					(float)(deformChangedMaxY - deformChangedMinY + 1) * texel / kUnitsPerMeter,
					(cx - half) / kUnitsPerMeter, (cy - half) / kUnitsPerMeter);
			}
		}

		if (deformIdleSkipped) {
			if (deformSkipRate >= 0.0f)
				ImGui::Text("Update pass: idle (skipped) - %.0f%% of last 300 frames", deformSkipRate * 100.0f);
			else
				ImGui::Text("Update pass: idle (skipped)");
		} else {
			static const char* kBlockerNames[8] = { "scroll", "stamps", "waves", "inject", "refill", "clear", "map-active", "verdict-stale" };
			std::string held;
			for (int bit = 0; bit < 8; bit++)
				if (deformIdleBlockers & (1u << bit)) {
					if (!held.empty())
						held += ", ";
					held += kBlockerNames[bit];
				}
			if (debugForceDeformationUpdate)
				held = held.empty() ? "forced" : "forced, " + held;
			if (deformSkipRate >= 0.0f)
				ImGui::Text("Update pass: running (%s) - skipped %.0f%% of last 300 frames",
					held.empty() ? "none - engages next verdict" : held.c_str(), deformSkipRate * 100.0f);
			else
				ImGui::Text("Update pass: running (%s)", held.empty() ? "none - engages next verdict" : held.c_str());
			// Within a running frame the evolve pass has its own gate: idle
			// means its last full run changed nothing and no external write
			// (ring inject, stamps) or refill has re-armed it - the ring and
			// stamp passes are the only cost while walking settled ground.
			ImGui::Text("Evolve pass: %s", evolveIdleLastFrame ? "idle (settled)" : "active");
		}
		if (deformSkipRate >= 0.0f) {
			// The flicker census: count changes are stamps appearing or
			// vanishing (plant-band flicker), drift is a matched stamp moving
			// past tolerance. Either resets the quiet window.
			ImGui::Text("Stamp set changes: %u/300 frames (count %u, drift %u)",
				stampSetCountChanges + stampSetDriftChanges, stampSetCountChanges, stampSetDriftChanges);
		}
		{
			// The tile census: dispatch domain vs the whole map. Cost should
			// track these counts; if it does not, the force-all-dirty toggle
			// is the discriminator.
			const uint32_t totalTiles = (deformMapDim / 8) * (deformMapDim / 8);
			if (debugForceAllTilesDirty)
				ImGui::Text("Stamp tiles: forced to full map (%u tiles)", totalTiles);
			else if (stampTilesLast > kStampTileCap)
				ImGui::Text("Stamp tiles: OVERFLOWED to full map (%u tiles)", totalTiles);
			else
				ImGui::Text("Stamp tiles: %u of %u", stampTilesLast, totalTiles);
			// Occupied + slump halo and changed + tap halo, from the last
			// executed scans (lag by the readback ring, like the verdict).
			ImGui::Text("Evolve tiles: %u of %u", evolveTilesLast, totalTiles);
			ImGui::Text("Berm tiles: %u of %u", bermTilesLast, totalTiles);
		}

		{
			// One tile is 512 world units square, only trodden ground has one,
			// and refill deletes the ones it takes back to bare snow. Occupancy
			// separates real trails from tiles a shallow refill residue is
			// keeping alive: a high thin count wants a bigger store epsilon,
			// not a faster fade.
			const auto stats = GetTrenchStoreStats();
			ImGui::Text("Trench store: %zu tiles, %.1f KB raw, %.1f%% full, %zu thin",
				stats.tiles, (double)stats.bytes / 1024.0, stats.occupancy * 100.0f, stats.thin);
			// Encoded is the number that matters: it is what a save will cost
			// once Stage C writes it, and what the budget is spent in.
			ImGui::Text("Save cost: %.0f KB of %.0f KB budget (%.0fx vs raw)",
				(double)stats.encoded / 1024.0,
				(double)settings.TrenchMemoryMB * 1024.0,
				stats.encoded ? (double)stats.bytes / (double)stats.encoded : 0.0);
		}

		ImGui::SeparatorText(T(TKEY("debug_cat_accumulation"), "Snow Accumulation"));

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

			auto* tes = RE::TES::GetSingleton();
			const bool indoors = tes && tes->interiorCell;

			// The scale the shells are actually handed, not what the peak would
			// give: with the toggle off it reads x1.000, which is the other
			// half of the A/B saying so.
			ImGui::Text("Accumulation: %.3f (depth x%.3f%s), snowfall %.2f%s",
				accum, GetAccumulationDepthScale(),
				settings.EnableSnowAccumulation ? "" : ", not applied", intensity,
				indoors ? " (held, indoors)" : "");
			ImGui::Text("Rate: %+.4f/game hour (grow %.4f, melt %.4f, fade %.4f)",
				rate, growth, melt, fade);
			ImGui::Text("Clock: %.2f game hours, timescale %.0f",
				gameClock.lastHours, gameClock.timescale);
			ImGui::Text("Co-save: %s", settings.PersistAccumulation ? "remembered" : "not written");
		}

		ImGui::SeparatorText(T(TKEY("debug_cat_spells"), "Spell Integration"));

		if (ImGui::TreeNodeEx(T(TKEY("spell_cat_stats"), "Detected"))) {
			// Diagnostics use plain text by existing convention (no i18n).
			ImGui::Text("projectiles %u | streams %u | hazards %u | cloaks %u | ground hits %u | trails %u",
				spellStats.projectiles, spellStats.streams, spellStats.hazards, spellStats.auras,
				spellStats.groundContacts, spellStats.trails);
			ImGui::Text("blasts: armed %u | detonations %u | casts %u | shouts %u (%u discs)   [totals since load]",
				spellStats.armed, spellStats.detonations, spellStats.casts,
				spellStats.shouts, spellStats.shoutDiscs);
			ImGui::Text("dash watches %u | furrows cut %u | travelling shoves %u | frost effects raised %u",
				spellStats.dashWatches, spellStats.dashGouges, spellStats.forceTracks, spellStats.lifted);
			if (spellStats.lastShoutVerdict) {
				static const char* kElem[] = { "none", "fire", "frost", "shock", "force" };
				static const char* kVerdict[] = { "-", "WEDGE", "TRACK", "rejected" };
				ImGui::Text("last shout: %s | proj speed %.0f | impact force %.0f | %s",
					spellStats.lastShoutElement < IM_ARRAYSIZE(kElem) ? kElem[spellStats.lastShoutElement] : "?",
					spellStats.lastShoutSpeed, spellStats.lastShoutForce,
					spellStats.lastShoutVerdict < IM_ARRAYSIZE(kVerdict) ? kVerdict[spellStats.lastShoutVerdict] : "?");
			}
			ImGui::Text("rejected: no element %u | no blast form %u",
				spellStats.rejectedElement, spellStats.rejectedNoBlast);
			ImGui::Text("innate auras %u | bodies burning %u | marking corpses %u | floating %u | translucent %u (neither carving)",
				spellStats.innate, spellStats.burning, spellStats.corpses, stampStats.floating, stampStats.incorporeal);
			ImGui::Text("death events seen %u | death blasts opened %u",
				spellStats.deathsSeen, spellStats.deathBlasts);
			if (stampStats.nearestValid) {
				static const char* kStateNames[] = { "on ground", "jumping", "in air", "climbing", "flying", "swimming" };
				const uint state = stampStats.nearestState;
				ImGui::Text("nearest actor: %s | gap to its own footing %.0f | gap to land %.0f | state %s",
					stampStats.nearestFloating ? "FLOATING" : "touching",
					stampStats.nearestGapToRoot, stampStats.nearestGapToLand,
					state < IM_ARRAYSIZE(kStateNames) ? kStateNames[state] : "none");
				ImGui::Text("             bones: feet %u usable %u%s | dry travel %.0f | limbs %u        (frame totals: feet %u | limbs %u | shapes %u | props %u | failsafe actors %u)",
					stampStats.nearestFeet, stampStats.nearestUsableFeet,
					stampStats.nearestFallback ? " FAILSAFE" : "",
					stampStats.nearestDryTravel, stampStats.nearestLimbs,
					stampStats.feet, stampStats.limbs, stampStats.shapes, stampStats.props,
					stampStats.fallbackActors);
				ImGui::Text("             body alpha %s | marked Ghost %s | verdict %s",
					stampStats.nearestElemental ? "not read" : std::format("{:.2f}", stampStats.nearestBodyAlpha).c_str(),
					stampStats.nearestGhostFlag ? "yes" : "no",
					stampStats.nearestIncorporeal ? "INCORPOREAL" :
						(stampStats.nearestElemental ? "solid (made of an element)" : "solid"));
			}
			ImGui::Text("emitters %u | awaiting their step %u | last mark: strength %.2f radius %.0f",
				spellStats.emitters, spellStats.pending, spellStats.lastStrength, spellStats.lastRadius);
			ImGui::Text("budget: actors+props %u/%u | spells %u/%u | emitters culled by distance %u | actors turned away %u",
				stampStats.beforeSpells, kMaxStamps - kSpellStampReserve,
				stampStats.spells, kSpellStampReserve, spellStats.emittersCulled,
				stampStats.budgetTurnedAway);
			ImGui::TreePop();
		}

		ImGui::SeparatorText(T(TKEY("debug_cat_melt_emitter"), "Melt Emitter"));
		if (auto _ttEmitterCat = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("debug_cat_melt_emitter_tooltip"), "A stand-in heat source that answers to no spell at all. Kept for testing a mark on its own: when a school stops marking, this says whether the fault is in the detector or in the mark itself."));
		if (ImGui::Button(T(TKEY("melt_emitter_drop"), "Drop Melt Emitter Here"))) {
			if (auto* player = RE::PlayerCharacter::GetSingleton()) {
				debugMeltEmitterPos = player->GetPosition();
				debugMeltEmitterActive = true;
			}
		}
		ImGui::SameLine();
		if (ImGui::Button(T(TKEY("melt_emitter_remove"), "Remove")))
			debugMeltEmitterActive = false;
		ImGui::Text("%s", debugMeltEmitterActive ?
							  T(TKEY("melt_emitter_active"), "Emitter: active") :
							  T(TKEY("melt_emitter_off"), "Emitter: off"));

		ImGui::SliderFloat(T(TKEY("melt_emitter_radius"), "Emitter Radius"), &debugMeltEmitterRadius, 40.0f, 600.0f, "%.0f");

		ImGui::SliderFloat(T(TKEY("melt_emitter_rate"), "Emitter Melt Rate"), &debugMeltEmitterRate, 0.02f, 2.0f, "%.2f /s");
		if (auto _ttMeltRate = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("melt_emitter_rate_tooltip"), "Depth melted per second at the bowl core. At 1.0 the core reaches full depth in a second; low values make the deepening easy to watch."));

		ImGui::SeparatorText(T(TKEY("debug_cat_shell"), "Shell & Terrain Data"));

		ImGui::Checkbox(T(TKEY("shell_data_debug"), "Shell: Data Debug Plane"), &shellDataDebug);

		ImGui::Checkbox(T(TKEY("shell_exclusion_debug"), "Shell: Exclusion Debug Plane"), &shellExclusionDebug);
		ImGui::Checkbox(T(TKEY("shell_border_debug"), "Shell: Border Debug Plane"), &shellBorderDebug);
		ImGui::Checkbox(T(TKEY("shell_sss_debug"), "Shell: SSS Gate Debug"), &shellSSSDebug);
		ImGui::Checkbox(T(TKEY("shell_wall_debug"), "Shell: Wall Material Debug"), &shellWallDebug);
		if (auto _ttWallDbg = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("shell_wall_debug_tooltip"), "Renders the shell's raw snow texture unlit, on the real geometry - no sun, shadows, glints or marches; red wash = how much the side projection owns the pixel. Strafe past a trench wall in this view: if the wall still shifts HERE the texture path is guilty; if this view is rock-solid, a lighting term is."));
		if (auto _ttSssDbg = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("shell_sss_debug_tooltip"), "Paints the Screen-Space Shadows gate on the shell. RED = how dark the mask (marched on the ground BENEATH the snow) wants this pixel. GREEN = how much the vertical hug gate trusts it. BLUE = the buried-caster probe found a captured object sunward and killed it. A shadow print = red + green with no blue. All black = the mask never reaches the shell here."));
		if (auto _ttBdbg = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("shell_border_debug_tooltip"), "False-color plane of the border fields: dark red = designed bare (below -0.5), orange = slice ribbon zone (-0.5..1), green = the cut zone (1..3, white line at the cut contour), cyan/blue = deeper snow. Brightness = snow grain; magenta grid = land grain data present at that pixel."));
		if (auto _ttExcl = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("shell_exclusion_debug_tooltip"), "Paints the exclusion channels on the debug plane: red = drift bank lift, green = melt fraction (fires, workspaces, sheltered ground), blue = door suppression. Black = untouched. The Data Debug Plane wins when both are on."));
		if (auto _ttPlane = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("shell_data_debug_tooltip"), "Renders the shell as an always-visible conforming plane colored by the terrain data it samples: red = height, green = snow coverage, blue = ramp depth. Black = no data reaches the shader."));

		ImGui::Checkbox(T(TKEY("shell_distant_exclusions_disabled"), "Shell: Disable Distant Clearings"), &shellDistantExclusionsDisabled);
		if (auto _ttExclusionField = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("shell_distant_exclusions_disabled_tooltip"), "Comparison aid: gates the wide exclusion field off, so campfire, workspace, bedroll and doorway clearings again stop at the object height window (about 57 m) and snow closes over them beyond it. Shelter under roofs and tents is unaffected either way - it needs the near window's geometry render."));

		ImGui::Checkbox(T(TKEY("shell_berm_bake_disabled"), "Shell: Disable Berm Bake"), &shellBermBakeDisabled);
		if (auto _ttBerm = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("shell_berm_bake_disabled_tooltip"), "Measurement aid: returns both shells to recomputing the berm field's 17 taps per call instead of reading the baked map, and skips the bake pass. The snow looks the same; Shell and Object Snow get slower and the BermField pass disappears. Hold the camera still and toggle to read the trade."));

		ImGui::Checkbox(T(TKEY("shell_bilinear_height"), "Shell: Bilinear Terrain Height"), &shellBilinearHeight);
		if (auto _ttBilin = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("shell_bilinear_height_tooltip"), "Measurement aid: returns the shell's terrain height to plain bilinear. Bilinear is the average of a quad's two possible triangulations, so it sits BELOW whichever one the landscape mesh uses - by tens of units on a steep saddle, which is deeper than the snow layer. Turn this on and poke-through should reappear on steep ground; off, the height follows the mesh and cannot sink under it."));

		ImGui::Checkbox(T(TKEY("shell_split_disabled"), "Shell: Disable Split Draw"), &shellSplitDisabled);
		if (auto _ttSplit = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("shell_split_disabled_tooltip"), "Measurement aid: returns the shell to a single draw that exports depth everywhere, which is how it worked before the split. With the split on, patches inside the far-clamp distance are drawn by a shader with no depth export so the GPU can reject hidden pixels before shading them, and only the far field keeps the export. The snow looks the same either way; hold the camera still and toggle to read what the split is worth. Does nothing while Depth Clamp is off - that is already a single no-export draw."));

		ImGui::Checkbox(T(TKEY("shell_depth_clamp_disabled"), "Shell: Disable Depth Clamp"), &shellDepthClampDisabled);
		if (auto _ttClampDbg = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("shell_depth_clamp_disabled_tooltip"), "Measurement aid, demoted from a setting: drops the shell's SV_DepthLessEqual export outright - one no-export draw, early-Z everywhere, and NO far-field clamp, so distant z-fighting returns while it is on. With the split draw on by default there is no configuration where this is a good trade; it exists to A/B what the clamp costs. Recompiles the shell PS on toggle."));

		ImGui::Checkbox(T(TKEY("statics_earlyz_spike"), "Object Snow: Drop Depth Export (early-Z spike)"), &staticsEarlyZSpike);
		if (auto _ttEZS = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("statics_earlyz_spike_tooltip"), "Measurement aid: forces the no-depth pixel shader onto EVERY object-snow draw, roads included. Normally only draws that can carve keep the depth export, which is already the bulk of the win at no visual cost; this shows the remaining ceiling. UPPER BOUND, not a clean A/B - the carve projects its parallax hit into that depth, so on roads this changes which pixels survive as well as what they cost, and their trench relief goes flat while it is on."));

		ImGui::Checkbox(T(TKEY("debug_overlay"), "Debug Terrain Overlay"), &debugTerrainOverlay);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("debug_overlay_tooltip"), "Paints diagnostics on terrain: red = outside deformation window, green = deformation, blue = detected snow."));

		ImGui::Checkbox(T(TKEY("debug_tiling_ruler"), "Debug Tiling Ruler"), &debugTilingRuler);
		if (auto _ttRuler = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("debug_tiling_ruler_tooltip"), "Measurement aid: draws three gridlines on the landscape. Red = one landscape texture repeat, green = 256 world units (the snow shell's tile), blue = 4096 (cell boundary). Counting red lines per green cell gives the shell-to-landscape tiling ratio directly; the blue lines are the scale anchor. Look straight down at flat ground near the camera."));

		ImGui::Checkbox(T(TKEY("debug_proj_snow"), "Debug Projected Snow Match"), &debugProjSnowView);
		if (auto _ttProj = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("debug_proj_snow_tooltip"), "Tints every pixel the projected-snow match classifies and replaces in magenta. If a snowy rock or fence shows no magenta, the classification missed that draw; if the magenta area is wrong, the projection weight is. The counters below break last frame's draws down; every projected material record seen is also logged to CommunityShaders.log."));
		ImGui::Text("Projected match, last frame: %u classified / %u no projection / %u vetoed",
			statProjMatchedPrev, statProjNoProjectionPrev, statProjVetoedPrev);

		ImGui::Checkbox(T(TKEY("debug_glacier_snow"), "Debug Glacier Snow Match"), &debugGlacierView);
		if (auto _ttGlac = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("debug_glacier_snow_tooltip"), "Paints every ice-family pixel the recolor reaches: BLUE = classified but kept as ice (snow mask 0), CYAN = recolored as snow (mask 1). A glacier with no tint at all means its draw never reached the recolor. The object snow shell draws on top and hides the tint where it covers."));

		ImGui::SeparatorText(T(TKEY("debug_cat_object_snow"), "Object Snow"));

		{
			const char* staticsDebugModes[] = { "Off", "Edge taper", "Coverage alpha", "Normals", "Self-shadow march", "Projected mask" };
			ImGui::Combo(T(TKEY("statics_debug_view"), "Object Snow Debug View"), &staticsDebugView, staticsDebugModes, IM_ARRAYSIZE(staticsDebugModes));
			if (auto _ttSdv = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("statics_debug_view_tooltip"), "Object snow renders its decision data as colors with dithering disabled; missing pixels mean the geometry itself is absent. The trench patch always reads red = trample, green = skin depth (dim) plus the road-heightfield bit (bright green, above half, means this column is road-classified). The skins follow the selected mode. Edge taper: red = the height the taper allows, green = up-facing, blue = the raster returned no data. Coverage alpha: red = the opacity the dither sees, green = the facing gates, blue = the seam blends. Normals: red = smoothed normal z (0.5 = horizontal, 1 = straight up), green = the flat/rounded class. Self-shadow march (patch and skins alike): red = how much the march darkens the pixel, green = taps that rebuilt the road's carved surface, blue = taps that used the flat dusting, dim magenta = the march never ran here (already shadowed, or the sun too low). Projected mask (skins only, patch renders dim gray): red = the current up-facing mask, green = vanilla's projected-snow weight rebuilt from the mesh's authored vertex alpha, so yellow = the two agree, red-only = only our normal test wants snow here; blue tint = the draw has no projected-UV data, in which case green is just the plain up-test and proves nothing."));
		}

		ImGui::SeparatorText(T(TKEY("debug_cat_lod"), "Distant Snow & LOD"));

		{
			std::string lodModes;
			lodModes += T(TKEY("lod_debug_off"), "Off");
			lodModes += '\0';
			lodModes += T(TKEY("lod_debug_heatmap"), "Depth Delta Heatmap");
			lodModes += '\0';
			lodModes += T(TKEY("lod_debug_rings"), "Vertex Spacing Bands");
			lodModes += '\0';
			lodModes += T(TKEY("lod_debug_provenance"), "Terrain Data Provenance");
			lodModes += '\0';
			ImGui::Combo(T(TKEY("lod_debug_view"), "Distant Debug View"), &lodDebugView, lodModes.c_str());
			if (auto _ttLod = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("lod_debug_view_tooltip"), "Heatmap: colors the shell by its vertical gap to the rendered ground (reds = buried, yellow = z-fight range, greens/blues = clearance) and fills the histogram below. Band View: the shell's VERTEX SPACING, not its depth - gray 8 units, yellow 16, green 32, cyan 64, blue 128 (the landscape's own vertex spacing), magenta coarser still. Each band brightens toward its outer edge. The bands are world-anchored: they must sit still on the ground as you move, and stripes that crawl mean the band table has lost its lattice alignment. Provenance: green = baked terrain data, red sheet = no data (unvisited cells)."));

			// Diagnostics below use plain text by existing convention (no i18n).
			if (lodDebugView == 1) {
				static const char* kBandLabels[kLODHistBands] = { "0-4k", "4-8k", "8-16k", "16k+" };
				static const char* kBucketLabels[kLODHistBuckets] = { "<-32", "-32..-8", "-8..-2", "+-2", "2..8", "8..32", "32..128", ">128" };
				if (ImGui::BeginTable("##lodhist", kLODHistBuckets + 2, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::Text("units");
					ImGui::TableNextColumn();
					ImGui::Text("pixels");
					for (uint32_t bucketI = 0; bucketI < kLODHistBuckets; ++bucketI) {
						ImGui::TableNextColumn();
						if (bucketI == 3)
							ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "%s", kBucketLabels[bucketI]);
						else
							ImGui::Text("%s", kBucketLabels[bucketI]);
					}
					for (uint32_t bandI = 0; bandI < kLODHistBands; ++bandI) {
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						ImGui::Text("%s", kBandLabels[bandI]);
						uint64_t bandTotal = 0;
						for (uint32_t bucketI = 0; bucketI < kLODHistBuckets; ++bucketI)
							bandTotal += lodHistData[bandI * kLODHistBuckets + bucketI];
						// Raw sample size: percent-only misleads when a band
						// holds a handful of pixels.
						ImGui::TableNextColumn();
						ImGui::Text("%llu", (unsigned long long)bandTotal);
						for (uint32_t bucketI = 0; bucketI < kLODHistBuckets; ++bucketI) {
							ImGui::TableNextColumn();
							const float pct = bandTotal ? 100.0f * lodHistData[bandI * kLODHistBuckets + bucketI] / bandTotal : 0.0f;
							if (bucketI == 3 && pct >= 0.05f)
								ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "%.1f%%", pct);
							else
								ImGui::Text("%.1f%%", pct);
						}
					}
					ImGui::EndTable();
				}
				ImGui::Text("Rows: camera distance bands. Columns: shell minus rendered ground, world units (share of band pixels).");
			}

			ImGui::Checkbox(T(TKEY("lod_no_far_pad"), "A/B: No Far Height Pad"), &lodDebugNoFarPad);
			if (auto _ttNoPad = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("lod_no_far_pad_tooltip"), "Disables the far-field height pad (the neighbour-max plus ridge pad past 3000 units), which scales with VIEWING DISTANCE and so moves distant ground as you walk toward it. Off is the shipped behaviour. Expect pinholes to come back if the pad was the thing hiding them - read the shimmer meter, not the holes."));

			ImGui::Checkbox(T(TKEY("lod_no_data_morph"), "A/B: No Data Morph"), &lodDebugNoDataMorph);
			if (auto _ttNoMorph = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("lod_no_data_morph_tooltip"), "Disables the coarse-lattice data morph, so every shell vertex reads its own fine-lattice terrain height. Off is the shipped behaviour. The morph blends height by ring index, which is a camera-distance term, so this is the other half of the distant up/down test."));

			ImGui::Checkbox(T(TKEY("lod_shimmer"), "Far-Field Shimmer Meter"), &lodShimmerMeter);
			if (auto _ttShimmer = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("lod_shimmer_tooltip"), "Evaluates the shell mesh surface at fixed world-anchored probe rings each frame and plots the frame-to-frame height change per distance band. Move the camera: spikes are vertex hops (the distant up/down shifting). Near-zero everywhere = stable far field."));
			if (lodShimmerMeter) {
				static const char* kShimmerBands[kLODHistBands] = { "0-4k", "4-8k", "8-16k", "16k+" };
				if (ImGui::Button("Reset Shimmer Window")) {
					for (uint32_t bandI = 0; bandI < kLODHistBands; ++bandI) {
						lodShimmerRunMax[bandI] = 0.0f;
						lodShimmerRunSum[bandI] = 0.0;
						lodShimmerRunCnt[bandI] = 0;
						lodShimmerRunHops[bandI] = 0;
					}
					lodShimmerRunFrames = 0;
					lodSeamChanges = 0;
					lodWindowRebuilds = 0;
				}
				ImGui::SameLine();
				ImGui::Text("%u frames measured", lodShimmerRunFrames);
				for (uint32_t bandI = 0; bandI < kLODHistBands; ++bandI) {
					const double runAvg = lodShimmerRunCnt[bandI] ? lodShimmerRunSum[bandI] / (double)lodShimmerRunCnt[bandI] : 0.0;
					char overlay[128];
					snprintf(overlay, sizeof(overlay), "%s: PEAK %.2f  mean %.3f  hops %u  (now %.2f, %u valid)",
						kShimmerBands[bandI], lodShimmerRunMax[bandI], runAvg, lodShimmerRunHops[bandI],
						lodShimmerMax[bandI], lodShimmerValid[bandI]);
					char plotId[16];
					snprintf(plotId, sizeof(plotId), "##shim%u", bandI);
					ImGui::PlotLines(plotId, lodShimmerHistoryBuf[bandI], kLODShimmerHistory, lodShimmerHistoryIdx,
						overlay, 0.0f, 25.0f, ImVec2(0.0f, 40.0f));
				}
				ImGui::Text("PEAK/mean/hops accumulate since Reset - screenshot THOSE, not 'now'. Pause frames publish nothing now, so an all-zero row means a still camera, never a stable shell.");
				ImGui::Text("Discrete events since reset: seam square %u, terrain window rebuilds %u", lodSeamChanges, lodWindowRebuilds);
			}
		}

		ImGui::SeparatorText(T(TKEY("debug_cat_stats"), "Statistics"));
		// Diagnostics: plain text by existing convention (no i18n).
		ImGui::Text("Stamps/frame: feet %u, limbs %u, shapes %u, props %u (prop refs %u, movers %u)",
			stampStats.feet, stampStats.limbs, stampStats.shapes, stampStats.props,
			stampStats.propRefs, stampStats.propMovers);
		ImGui::Text("Snow statics captured: %u", statCapturedStatics.load(std::memory_order_relaxed));
		ImGui::Text("Snowfall intensity: %.2f (refill %s)", snowfallIntensity,
			settings.RefillOnlyWhenSnowing ? "weather-driven" : "baseline");
		{
			static const char* kSnowGateNames[] = {
				"running (snowy cell in reach)",
				"running (unbaked ground in reach, assumed snow)",
				"suspended (all ground in reach known bare)"
			};
			ImGui::Text("Snow presence gate: %s", kSnowGateNames[std::min(deformSnowVerdict, 2u)]);
		}

		ImGui::Checkbox("Skeleton Probe (nearest NPC)", &debugSkeletonProbe);
		if (debugSkeletonProbe) {
			ImGui::SameLine();
			if (ImGui::Button("Dump skeleton to log"))
				skeletonProbeDumpRequested = true;
			if (!skeletonProbe.valid) {
				ImGui::Text("probe: no NPC in range this frame");
			} else {
				ImGui::Text("probe: %s (%08X) - %s",
					skeletonProbe.actorName.empty() ? "<unnamed>" : skeletonProbe.actorName.c_str(),
					skeletonProbe.formID, skeletonProbe.verdict);
				ImGui::Text("  feet %zu (usable %u) | limbs %u (stamped %u) | shapes stamped %u | dry travel %.0f%s",
					skeletonProbe.feet.size(), skeletonProbe.usableFeet,
					skeletonProbe.limbs, skeletonProbe.limbsStamped, skeletonProbe.shapes,
					skeletonProbe.dryTravel, skeletonProbe.collisionFallback ? " [FAILSAFE]" : "");
				ImGui::Text("  body alpha %.2f (reads %u) | above land %.0f | floating gap %.0f",
					skeletonProbe.bodyAlpha, (uint)skeletonProbe.alphaSettle,
					skeletonProbe.gapToLand, skeletonProbe.floatingGap);
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
		if (auto* sky = RE::Sky::GetSingleton())
			ImGui::Text("Wind: %.2f toward %.0f deg (drift-biased refill)", sky->windSpeed,
				Util::Units::RadiansToDegrees(sky->windAngle));
		ImGui::Text("Exclusion zones: %u, workspace clearings: %u, sealed containers: %u (Survival heat list %s)",
			statExclusionCount, statTrampleCount, statSealedCount, survivalHeatSources ? "found" : "absent");
		ImGui::Text("Snow mask cache: %zu entries, %llu hits, %llu misses",
			snowMasksSizeForUI(),
			(unsigned long long)landMaskHits.load(std::memory_order_relaxed),
			(unsigned long long)landMaskMisses.load(std::memory_order_relaxed));
		ImGui::Text("Terrain data: %zu cells baked, %u in window, %u snow texels, height range [%.0f, %.0f]",
			ShellCellCountForUI(), shellStatCellsInWindow, shellStatSnowTexels,
			shellStatMinHeight, shellStatMaxHeight);

		// Shell probe: what the landscape shell reads at the camera. A shell
		// artifact that this call cannot account for is not the landscape
		// shell - it is object snow, which the Object Snow debug view owns.
		{
			auto eye = globals::game::frameBufferCached.GetCameraPosAdjust();
			const ShellProbe probe = ProbeShellData(eye.x, eye.y);
			const char* worldspaceName = "none";
			if (auto* tes = RE::TES::GetSingleton())
				if (auto* worldspace = tes->GetRuntimeData2().worldSpace)
					worldspaceName = worldspace->GetFormEditorID();

			ImGui::Text("Shell probe @ (%.0f, %.0f) cell (%d, %d) vertex (%d, %d)",
				probe.worldX, probe.worldY, probe.cellX, probe.cellY, probe.vertexX, probe.vertexY);
			ImGui::Text("  worldspace %08X %s, window built for %08X",
				probe.activeWorldspaceID, worldspaceName, probe.windowWorldspaceID);
			if (!probe.cellFound) {
				ImGui::Text("  no baked cell: the shell has NO terrain data here");
			} else if (!probe.worldspaceMatch) {
				ImGui::Text("  cell baked in worldspace %08X: REJECTED, shell has no data here", probe.cellWorldspace);
			} else {
				ImGui::Text("  height %.0f, depth %.1f, coverage %.2f -> surface %.0f",
					probe.height, probe.rampDepth, probe.coverage, probe.height + probe.rampDepth);
				for (const auto& layer : probe.layers)
					ImGui::Text("    %s x%.2f @ %.0f units", layer.label.c_str(), layer.weight, layer.depth);
			}

			// Object snow probe: which captured meshes cover this spot, largest
			// first. Bound top Z against the camera height says whether one of
			// them is the surface an artifact sits on.
			const ObjectSnowProbe objects = ProbeObjectSnow(eye.x, eye.y);
			ImGui::Text("Object snow probe: %zu captured this frame, %zu cover this spot (camera z %.0f)",
				objects.captured, objects.overlapping, eye.z);
			for (const auto& entry : objects.entries)
				ImGui::Text("  %s r%.0f top %.0f (%.0f away)%s  %s",
					entry.name.empty() ? "<unnamed>" : entry.name.c_str(),
					entry.radius, entry.topZ, entry.distXY, entry.road ? " [road]" : "", entry.model.c_str());
		}
		ImGui::Text("Shadow source: descriptors=%u endSplits=%.0f/%.0f/%.0f atlasSlices=%u",
			dbgLodDescriptorCount, dbgLodEndSplits[0], dbgLodEndSplits[1], dbgLodEndSplits[2], dbgLodAtlasSlices);

		uint64_t vramUsageMB = 0, vramBudgetMB = 0;
		QueryAdapterVRAM(vramUsageMB, vramBudgetMB);
		std::string vramBreakdown;
		const uint64_t vramFeatureMB = SumFeatureTextureBytes(vramBreakdown) >> 20;
		ImGui::Text("VRAM: adapter %llu / %llu MB (%llu%%), this feature ~%llu MB",
			(unsigned long long)vramUsageMB, (unsigned long long)vramBudgetMB,
			(unsigned long long)(vramBudgetMB ? vramUsageMB * 100 / vramBudgetMB : 0),
			(unsigned long long)vramFeatureMB);
		if (vramBudgetMB && vramUsageMB > vramBudgetMB)
			ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "OVER BUDGET: driver is demoting textures to system RAM; FPS stays degraded until the game restarts.");
		ImGui::TextWrapped("%s", vramBreakdown.c_str());

		ImGui::TreePop();
	}
#endif
}
