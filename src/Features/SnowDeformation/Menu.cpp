#include "Features/SnowDeformation.h"

#include <imgui_stdlib.h>

#include "Utils/Game.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "feature.snow_deformation."

void SnowDeformation::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enable"), "Enable Snow Deformation"), &settings.EnableSnowDeformation);

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
		ImGui::Checkbox(T(TKEY("snow_texture_linear"), "Linear (PBR) Texture"), &settings.SnowTextureLinear);
		if (auto _ttLin = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("snow_texture_linear_tooltip"), "Legacy override: enable when a NON-PBR texture stores linear color. When a PBR set is auto-resolved (Textures\\PBR\\...), linear color is detected automatically and this checkbox is ignored."));

		ImGui::SliderFloat(T(TKEY("stamp_radius"), "Stamp Radius"), &settings.StampRadius, 4.0f, 128.0f, "%.0f");
		if (auto _ttStamp = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("stamp_radius_tooltip"), "Scales the Havok collision-shape radii used for stamping (20 = the shapes' actual size). Stamps come from actors' real collision shapes — feet and legs carve individually."));
		ImGui::SliderFloat(T(TKEY("footprint_width"), "Footprint Width"), &settings.FootPrintScale, 0.5f, 3.0f, "%.2f x");
		if (auto _ttFw = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("footprint_width_tooltip"), "Width multiplier on foot prints; length follows the skeleton. Snow collapses wider than the foot, so above 1.0 usually reads best."));
		ImGui::SliderFloat(T(TKEY("trench_sharpness"), "Trench Wall Sharpness"), &settings.TrenchWallSharpness, 0.0f, 100.0f, "%.0f %%");
		if (auto _ttSharp = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("trench_sharpness_tooltip"), "How steeply trench walls drop. Low = wide, soft banks; 100 = full depth held to the trail's very edge."));
		ImGui::SliderFloat(T(TKEY("trail_irregularity"), "Trail Irregularity"), &settings.TrailIrregularity, 0.0f, 1.0f, "%.2f");
		if (auto _ttIrr = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("trail_irregularity_tooltip"), "World-anchored noise wobbling every stamp's edge, so trails read as churned snow instead of swept circles."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("render_distance"), "Render Distance"), ImGuiTreeNodeFlags_Framed)) {
		if (auto _ttRd = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("render_distance_tooltip"), "How far each snow system reaches. Higher = more VRAM and GPU cost. The snow shell itself auto-sizes to the game's loaded-cell grid and hands off to Horizon Snow beyond it."));
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

	if (ImGui::TreeNodeEx(T(TKEY("distant_snow"), "Distant Snow"), ImGuiTreeNodeFlags_Framed)) {
		if (auto _ttDs = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("distant_snow_tooltip"), "Snow on far terrain the game hasn't loaded: heights come from the worldspace heightmap (shipped with Community Shaders), and snow placement follows the game's own distant LOD textures — where the LOD is painted snowy, our snow appears. Loaded terrain always uses its real snow textures instead."));
		ImGui::Checkbox(T(TKEY("horizon_snow"), "Horizon Snow"), &settings.HorizonSnow);
		if (auto _ttHs = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("horizon_snow_tooltip"), "Recolors the game's distant LOD terrain with the shell's own snow material wherever its bake reads as snow, so snow appearance stays consistent from your feet to the horizon. The snow shell ends at the loaded-cell boundary and this takes over from there, out to the edge of the world."));
		bool distantChanged = false;
		distantChanged |= ImGui::SliderFloat(T(TKEY("lod_snow_sensitivity"), "LOD Snow Detection"), &settings.LODSnowSensitivity, 0.0f, 1.0f, "%.2f");
		if (auto _ttLss = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("lod_snow_sensitivity_tooltip"), "How eagerly a distant LOD texture pixel counts as snow. The scale was widened: the old best-at-1.0 now sits near 0.5. Low = only bright white; high = pale gray rock starts counting too. Check with the Terrain Data Provenance debug view (brown = bare, blue-white = snow); the same setting drives the Horizon Snow recolor."));
		ImGui::TextDisabled("%s", T(TKEY("distant_snow_fallback_label"), "Fallback snow line (used only where LOD textures are missing):"));
		distantChanged |= ImGui::SliderFloat(T(TKEY("distant_snow_line"), "Snow Line Height"), &settings.DistantSnowLineZ, -10000.0f, 30000.0f, "%.0f units");
		if (auto _ttDsl = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("distant_snow_line_tooltip"), "Elevation above which distant unloaded terrain reads as snow-covered, where no LOD terrain texture exists to read the answer from."));
		distantChanged |= ImGui::SliderFloat(T(TKEY("distant_snow_north"), "North Snow Drop"), &settings.DistantSnowNorthDrop, 0.0f, 40000.0f, "%.0f units");
		if (auto _ttDsn = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("distant_snow_north_tooltip"), "How far the fallback snow line sinks toward the map's north edge, so the northern coast is snowy at sea level while southern plains at the same elevation stay bare."));
		distantChanged |= ImGui::SliderFloat(T(TKEY("distant_snow_fade"), "Snow Line Fade"), &settings.DistantSnowLineFade, 100.0f, 6000.0f, "%.0f units");
		if (auto _ttDsf = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("distant_snow_fade_tooltip"), "Width of the bare-to-snow transition band around the fallback snow line."));
		if (distantChanged)
			shellDataDirty.store(true, std::memory_order_release);
		ImGui::Separator();
		ImGui::SliderFloat(T(TKEY("range_skins_geometry"), "Object Snow Geometry Range"), &settings.RangeSkinsGeometryM, 10.0f, 200.0f, "%.0f m");
		if (auto _ttRkg = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("range_skins_geometry_tooltip"), "Distance where raised snow on objects flattens back into a painted layer. The layer's height sinks to zero before Distant Snow Blend starts dissolving it, so the switch has no silhouette to pop. Deep snow classes keep their height further out than thin ones. Higher values keep real snow depth further out at the cost of more geometry work."));
		ImGui::SliderFloat(T(TKEY("skin_distant_bareness"), "Distant Bare Rock"), &settings.SkinDistantBareness, 0.0f, 1.0f, "%.2f");
		if (auto _ttSdb = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("skin_distant_bareness_tooltip"), "How much bare rock distant cliffs and boulders keep. Close up, snow coverage follows the smoothed mesh normal, which on low-poly rocks reports steep flanks as up-facing; near the camera the edge taper hides that, but at range it turns a rock into a white blob. This hands the coverage test over to each face's true orientation as the object shrinks, so steep faces shed their snow again. Raise it for more exposed rock; too high and the mesh's own triangles start to read as jagged facets and seams. 0 keeps the old behaviour."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("snow_refill"), "Snow Refill"), ImGuiTreeNodeFlags_Framed)) {
		if (auto _ttRefillTree = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("snow_refill_tooltip"), "How compressed snow recovers and how raised snow settles."));
		ImGui::Checkbox(T(TKEY("refill_only_snowing"), "Refill Only While Snowing"), &settings.RefillOnlyWhenSnowing);
		if (auto _ttRefillSnow = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("refill_only_snowing_tooltip"), "Compressed snow only recovers while the current weather is snowing, faster in denser snowfall. Trails and trenches persist through clear weather. Off: snow recovers at the baseline rate in any weather."));
		ImGui::SliderFloat(T(TKEY("refill_rate"), "Snow Refill Rate"), &settings.RefillRateMultiplier, 0.0f, 10.0f, "%.1fx");
		if (auto _ttRefill = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("refill_rate_tooltip"), "Multiplier on the snowfall-driven refill rate. At 1.0x, typical snowfall recovers compressed snow in about 12 minutes. 0 disables refilling."));
		ImGui::SliderFloat(T(TKEY("mound_steepness"), "Mound Steepness"), &settings.SnowMoundSteepness, 0.5f, 3.0f, "%.1f");
		if (auto _ttSteep = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("mound_steepness_tooltip"), "Angle of repose for snow mounds (1.0 = 45 degrees). Steeper = raised snow clings tighter: narrow banks instead of broad aprons, juttier mounds."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("undulation"), "Surface Undulation"), ImGuiTreeNodeFlags_Framed)) {
		if (auto _ttUnd = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("undulation_tooltip"), "Wind-worked waves in deep snow. They fade out automatically over thin cover, class borders and carved trench floors."));
		ImGui::SliderFloat(T(TKEY("undulation_strength"), "Undulation Strength"), &settings.UndulationStrength, 0.0f, 8.0f, "%.1f units");
		if (auto _ttUs = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("undulation_strength_tooltip"), "Wave height. 0 flattens deep snow into a smooth sheet."));
		ImGui::SliderFloat(T(TKEY("undulation_spacing"), "Undulation Spacing"), &settings.UndulationSpacing, 0.5f, 4.0f, "%.1fx");
		if (auto _ttUsp = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("undulation_spacing_tooltip"), "Stretches the wave pattern: larger = broader, calmer dunes instead of a spike carpet."));
		ImGui::SliderFloat(T(TKEY("relief_depth"), "Relief Depth"), &settings.ReliefDepth, 0.0f, 12.0f, "%.1f units");
		if (auto _ttRd2 = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("relief_depth_tooltip"), "Real geometric relief from the snow texture's displacement map, tessellated near the camera. Compressed snow and trench floors stay smooth. 0 disables tessellation."));
		ImGui::SliderFloat(T(TKEY("parallax_shadow_strength"), "Parallax Shadow"), &settings.ParallaxShadowStrength, 0.0f, 2.0f, "%.2fx");
		if (auto _ttPss = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("parallax_shadow_strength_tooltip"), "Self-shadowing of the snow's own grain, the same term PBR ground receives from Extended Materials: four taps along the sun through the displacement map, so the micro-relief casts into itself under low sun instead of reading flat. Needs the PBR snow set's _p map. 0 skips the taps entirely (and is the A/B for their cost)."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("model_depths"), "Snow Depth by Model Class"), ImGuiTreeNodeFlags_Framed)) {
		if (auto _ttModels = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("model_depths_tooltip"), "Snow layer height per OBJECT model class. Roads are matched by their road/bridge names and textures; flat vs round is classified automatically per mesh."));
		ImGui::SliderFloat(T(TKEY("road_meshes_depth"), "Road Meshes"), &settings.RoadMeshesDepth, 0.0f, 64.0f, "%.0f units");
		ImGui::Checkbox(T(TKEY("object_trenches"), "Trenches on Objects"), &settings.ObjectTrenches);
		if (auto _ttOt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("object_trenches_tooltip"), "Carve footprints into snow sitting on objects (rocks, logs, roofs). Off while the object trenching is being reworked; roads and bridges keep their trenches either way."));
		if (auto _ttRoad = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("road_meshes_depth_tooltip"), "Snow layer on road and bridge meshes. Kept below the surrounding snow classes so the road's course stays readable through the snowfield."));
		ImGui::SliderFloat(T(TKEY("objects_snow_depth"), "Flat Objects"), &settings.ObjectsSnowDepth, 0.0f, 25.0f, "%.0f units");
		if (auto _ttObj = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("objects_snow_depth_tooltip"), "Snow layer on flat hard-edged meshes (walkways, roofs, planks) — these get a completely flat overlay, no fake 3D. Classified automatically per mesh."));
		ImGui::SliderFloat(T(TKEY("snow_meshes_depth"), "Round Objects"), &settings.SnowMeshesDepth, 0.0f, 25.0f, "%.0f units");
		if (auto _ttMesh = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("snow_meshes_depth_tooltip"), "Snow layer on organically smooth meshes (rocks, drifts, logs), where the puffed pillow layer reads correctly in 3D."));
		ImGui::SliderFloat(T(TKEY("floor_see_through"), "Trench Floor See-Through"), &settings.TrenchFloorFade, 0.0f, 1.0f, "%.2f");
		if (auto _ttFloor = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("floor_see_through_tooltip"), "How much a heavily trampled trench floor on an object (rock, log, walkway) wears through to the object's own surface instead of holding solid snow."));
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
		ImGui::SliderFloat(T(TKEY("workspace_clearing_size"), "Workspace Clearing Size"), &settings.TrampleZoneScale, 0.25f, 2.0f, "%.2fx");
		if (auto _ttWcs = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("workspace_clearing_size_tooltip"), "Radius multiplier for the snow bowls around workstations, smelters, forges, stalls, wells and shrines. Applies within a second."));
		ImGui::SliderFloat(T(TKEY("workspace_clearing_height"), "Workspace Clearing Height"), &settings.TrampleZoneHeight, 0.0f, 100.0f, "%.0f%%");
		if (auto _ttWch = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("workspace_clearing_height_tooltip"), "Snow height remaining in a workspace bowl, as a percent of the surrounding depth. 0 melts to the floor, 100 disables the clearing. Applies within a second."));
		ImGui::SliderFloat(T(TKEY("wall_drift_height"), "Wall Drift Height"), &settings.WallDriftHeight, 0.0f, 48.0f, "%.0f units");
		if (auto _ttWdh = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("wall_drift_height_tooltip"), "Peak height of snow banks drifted against buildings and other large structures. Windward walls bank fully with the weather's wind, calm weather keeps modest banks all around, and the leeward side stays scoured. 0 disables."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("trench_detail"), "Landscape Trenches"), ImGuiTreeNodeFlags_Framed)) {
		if (auto _ttTd = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("trench_detail_tooltip"), "The look of disturbed snow: the raised berm along trench edges, the chunky churned surface, and the fine-grain shading detail. Untouched snow is never affected."));
		ImGui::SliderFloat(T(TKEY("berm_height"), "Berm Height"), &settings.BermHeight, 0.0f, 1.0f, "%.2fx");
		if (auto _ttBh = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("berm_height_tooltip"), "Height of the pushed-aside snow ridge along trench edges, as a fraction of the local snow depth. 0 removes the berm."));
		ImGui::SliderFloat(T(TKEY("churn_height"), "Churn Height"), &settings.ChurnHeight, 0.0f, 8.0f, "%.1f units");
		if (auto _ttCh = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("churn_height_tooltip"), "How tall the broken snow lumps are in trenches and on berms. 0 leaves disturbed snow smooth."));
		ImGui::SliderFloat(T(TKEY("churn_size"), "Churn Size"), &settings.ChurnSize, 0.25f, 4.0f, "%.2fx");
		if (auto _ttCs = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("churn_size_tooltip"), "Size of the broken snow lumps: smaller = finer rubble, larger = broad clods."));
		ImGui::SliderFloat(T(TKEY("crisp_scale"), "Grain Fineness"), &settings.CrispScale, 1.0f, 8.0f, "%.1fx");
		if (auto _ttGf = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("crisp_scale_tooltip"), "How much finer the snow normal map repeats on disturbed snow (shading only)."));
		ImGui::SliderFloat(T(TKEY("crisp_strength"), "Grain Strength"), &settings.CrispStrength, 0.0f, 3.0f, "%.2f");
		if (auto _ttGs = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("crisp_strength_tooltip"), "How strongly the fine grain cuts through on disturbed snow. 0 disables it."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("obj_trench_detail"), "Object Trenches"), ImGuiTreeNodeFlags_Framed)) {
		if (auto _ttOtd = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("obj_trench_detail_tooltip"), "The same disturbed-snow detail for snow on objects (roads, rocks, logs), independent of the landscape set. The berm is shading-only here."));
		ImGui::SliderFloat(T(TKEY("obj_berm_height"), "Berm Height"), &settings.ObjBermHeight, 0.0f, 1.0f, "%.2fx");
		if (auto _ttObh = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("obj_berm_height_tooltip"), "Strength of the shaded snow ridge along object trails. 0 removes it."));
		ImGui::SliderFloat(T(TKEY("obj_churn_height"), "Churn Height"), &settings.ObjChurnHeight, 0.0f, 8.0f, "%.1f units");
		if (auto _ttOch = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("obj_churn_height_tooltip"), "How tall the broken lumps are in object trench walls. Floors keep their thin cover regardless."));
		ImGui::SliderFloat(T(TKEY("obj_churn_size"), "Churn Size"), &settings.ObjChurnSize, 0.25f, 4.0f, "%.2fx");
		if (auto _ttOcs = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("obj_churn_size_tooltip"), "Size of the broken lumps: smaller = finer rubble, larger = broad clods."));
		ImGui::SliderFloat(T(TKEY("obj_crisp_scale"), "Grain Fineness"), &settings.ObjCrispScale, 1.0f, 8.0f, "%.1fx");
		if (auto _ttOgf = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("obj_crisp_scale_tooltip"), "How much finer the snow normal map repeats on disturbed object snow (shading only)."));
		ImGui::SliderFloat(T(TKEY("obj_crisp_strength"), "Grain Strength"), &settings.ObjCrispStrength, 0.0f, 3.0f, "%.2f");
		if (auto _ttOgs = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("obj_crisp_strength_tooltip"), "How strongly the fine grain cuts through on disturbed object snow. 0 disables it."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("debug_options"), "Debugging Options"), ImGuiTreeNodeFlags_Framed)) {
		ImGui::SeparatorText(T(TKEY("debug_cat_deform_map"), "Deformation Map"));
		ImGui::Checkbox(T(TKEY("show_debug"), "Show Deformation Map"), &settings.ShowDebugTexture);
		if (settings.ShowDebugTexture) {
			ImGui::Text("%s", T(TKEY("debug_hint"), "White = compressed snow. The map follows the camera."));
			ImGui::Image(GetDeformationSRV(), { 512.0f, 512.0f });
		}

		if (ImGui::Button(T(TKEY("clear"), "Clear Deformation Map")))
			clearRequested = true;

		ImGui::SeparatorText(T(TKEY("debug_cat_shell"), "Shell & Terrain Data"));
		ImGui::Checkbox(T(TKEY("shell_data_debug"), "Shell: Data Debug Plane"), &shellDataDebug);
		ImGui::Checkbox(T(TKEY("shell_exclusion_debug"), "Shell: Exclusion Debug Plane"), &shellExclusionDebug);
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

		ImGui::Checkbox(T(TKEY("debug_overlay"), "Debug Terrain Overlay"), &debugTerrainOverlay);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("debug_overlay_tooltip"), "Paints diagnostics on terrain: red = outside deformation window, green = deformation, blue = detected snow."));

		ImGui::Checkbox(T(TKEY("debug_tiling_ruler"), "Debug Tiling Ruler"), &debugTilingRuler);
		if (auto _ttRuler = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("debug_tiling_ruler_tooltip"), "Measurement aid: draws three gridlines on the landscape. Red = one landscape texture repeat, green = 256 world units (the snow shell's tile), blue = 4096 (cell boundary). Counting red lines per green cell gives the shell-to-landscape tiling ratio directly; the blue lines are the scale anchor. Look straight down at flat ground near the camera."));

		ImGui::SeparatorText(T(TKEY("debug_cat_object_snow"), "Object Snow"));
		{
			const char* staticsDebugModes[] = { "Off", "Edge taper", "Coverage alpha" };
			ImGui::Combo(T(TKEY("statics_debug_view"), "Object Snow Debug View"), &staticsDebugView, staticsDebugModes, IM_ARRAYSIZE(staticsDebugModes));
			if (auto _ttSdv = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("statics_debug_view_tooltip"), "Edge taper: red = height the slump allows, green = up-facing, blue = no height data. Coverage alpha: red = the opacity the dither sees, green = the facing gates, blue = the seam blends."));
		}
		if (auto _ttSdv = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("statics_debug_view_tooltip"), "Object snow renders its decision data as colors with dithering disabled. Trench patch: red = trample, green = skin depth. Skins: teal, brightness = up-facing coverage. Missing pixels mean the geometry itself is absent."));

		ImGui::SeparatorText(T(TKEY("debug_cat_lod"), "Distant Snow & LOD"));
		{
			std::string lodModes;
			lodModes += T(TKEY("lod_debug_off"), "Off");
			lodModes += '\0';
			lodModes += T(TKEY("lod_debug_heatmap"), "Depth Delta Heatmap");
			lodModes += '\0';
			lodModes += T(TKEY("lod_debug_rings"), "Warp Ring View");
			lodModes += '\0';
			lodModes += T(TKEY("lod_debug_provenance"), "Terrain Data Provenance");
			lodModes += '\0';
			ImGui::Combo(T(TKEY("lod_debug_view"), "Distant Debug View"), &lodDebugView, lodModes.c_str());
			if (auto _ttLod = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("lod_debug_view_tooltip"), "Heatmap: colors the shell by its vertical gap to the rendered ground (reds = buried, yellow = z-fight range, greens/blues = clearance) and fills the histogram below. Ring View: warp rings by color, dimmed while still camera-relative. Provenance: green = baked terrain data, red sheet = no data (unvisited cells)."));

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

			ImGui::Checkbox(T(TKEY("lod_shimmer"), "Far-Field Shimmer Meter"), &lodShimmerMeter);
			if (auto _ttShimmer = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("lod_shimmer_tooltip"), "Evaluates the shell mesh surface at fixed world-anchored probe rings each frame and plots the frame-to-frame height change per distance band. Move the camera: spikes are vertex hops (the distant up/down shifting). Near-zero everywhere = stable far field."));
			if (lodShimmerMeter) {
				static const char* kShimmerBands[kLODHistBands] = { "0-4k", "4-8k", "8-16k", "16k+" };
				for (uint32_t bandI = 0; bandI < kLODHistBands; ++bandI) {
					char overlay[96];
					snprintf(overlay, sizeof(overlay), "%s: max %.2f avg %.3f hops %u/%u", kShimmerBands[bandI],
						lodShimmerMax[bandI], lodShimmerAvg[bandI], lodShimmerHops[bandI], lodShimmerValid[bandI]);
					char plotId[16];
					snprintf(plotId, sizeof(plotId), "##shim%u", bandI);
					ImGui::PlotLines(plotId, lodShimmerHistoryBuf[bandI], kLODShimmerHistory, lodShimmerHistoryIdx,
						overlay, 0.0f, 25.0f, ImVec2(0.0f, 40.0f));
				}
				ImGui::Text("Per-frame max |dZ| (units, 0-25 scale). Deltas pause for one frame when the probe anchor requantizes (every 512 units of travel).");
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
		if (auto* sky = RE::Sky::GetSingleton())
			ImGui::Text("Wind: %.2f toward %.0f deg (drift-biased refill)", sky->windSpeed,
				Util::Units::RadiansToDegrees(sky->windAngle));
		ImGui::Text("Exclusion zones: %u, workspace clearings: %u, drift obstructions: %u (Survival heat list %s)",
			statExclusionCount, statTrampleCount, statObstructionCount, survivalHeatSources ? "found" : "absent");
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
}
