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

		ImGui::SliderFloat(T(TKEY("stamp_radius"), "Stamp Radius"), &settings.StampRadius, 4.0f, 128.0f, "%.0f");
		if (auto _ttStamp = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("stamp_radius_tooltip"), "Scales the Havok collision-shape radii used for stamping (20 = the shapes' actual size). Stamps come from actors' real collision shapes — feet and legs carve individually."));

		ImGui::SliderFloat(T(TKEY("footprint_width"), "Footprint Width"), &settings.FootPrintScale, 0.5f, 3.0f, "%.2f x");
		if (auto _ttFw = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("footprint_width_tooltip"), "Width multiplier on foot prints; length follows the skeleton. Snow collapses wider than the foot, so above 1.0 usually reads best."));

		ImGui::SliderFloat(T(TKEY("trench_sharpness"), "Trench Wall Sharpness"), &settings.TrenchWallSharpness, 0.0f, 100.0f, "%.0f %%");
		if (auto _ttSharp = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("trench_sharpness_tooltip"), "How steeply trench walls drop. Low = wide, soft banks; 100 = full depth held to the trail's very edge."));

		ImGui::PushID("general_settings");
		if (ImGui::TreeNodeEx(T(TKEY("menu_advanced"), "Advanced"))) {
			ImGui::Checkbox(T(TKEY("snow_texture_linear"), "Linear (PBR) Texture"), &settings.SnowTextureLinear);
			if (auto _ttLin = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("snow_texture_linear_tooltip"), "Legacy override: enable when a NON-PBR texture stores linear color. When a PBR set is auto-resolved (Textures\\PBR\\...), linear color is detected automatically and this checkbox is ignored."));

			ImGui::SliderFloat(T(TKEY("trail_irregularity"), "Trail Irregularity"), &settings.TrailIrregularity, 0.0f, 1.0f, "%.2f");
			if (auto _ttIrr = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("trail_irregularity_tooltip"), "World-anchored noise wobbling every stamp's edge, so trails read as churned snow instead of swept circles."));

			ImGui::TreePop();
		}
		ImGui::PopID();

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

		ImGui::PushID("render_distance");
		if (ImGui::TreeNodeEx(T(TKEY("menu_advanced"), "Advanced"))) {
			ImGui::SliderFloat(T(TKEY("range_skins_fade"), "Distant Snow Blend"), &settings.RangeSkinsFadeM, 29.0f, 750.0f, "%.0f m");
			if (auto _ttRkf = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("range_skins_fade_tooltip"), "Distance where object snow starts dissolving back into the object's own appearance; fully faded by the Object Snow range end. Cures distant blank-white objects."));

			ImGui::TreePop();
		}
		ImGui::PopID();

		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("distant_snow"), "Distant Snow"), ImGuiTreeNodeFlags_Framed)) {
		if (auto _ttDs = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("distant_snow_tooltip"), "Snow on far terrain the game hasn't loaded: heights come from the worldspace heightmap (shipped with Community Shaders), and snow placement follows the game's own distant LOD textures — where the LOD is painted snowy, our snow appears. Loaded terrain always uses its real snow textures instead."));
		ImGui::Checkbox(T(TKEY("horizon_snow"), "Horizon Snow"), &settings.HorizonSnow);
		if (auto _ttHs = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("horizon_snow_tooltip"), "Recolors the game's distant LOD terrain with the shell's own snow material wherever its bake reads as snow, so snow appearance stays consistent from your feet to the horizon. The snow shell ends at the loaded-cell boundary and this takes over from there, out to the edge of the world."));
		bool distantChanged = false;

		distantChanged |= ImGui::SliderFloat(T(TKEY("distant_snow_line"), "Snow Line Height"), &settings.DistantSnowLineZ, -10000.0f, 30000.0f, "%.0f units");
		if (auto _ttDsl = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("distant_snow_line_tooltip"), "Elevation above which distant unloaded terrain reads as snow-covered, where no LOD terrain texture exists to read the answer from."));

		ImGui::PushID("distant_snow");
		if (ImGui::TreeNodeEx(T(TKEY("menu_advanced"), "Advanced"))) {
			distantChanged |= ImGui::SliderFloat(T(TKEY("lod_snow_sensitivity"), "LOD Snow Detection"), &settings.LODSnowSensitivity, 0.0f, 1.0f, "%.2f");
			if (auto _ttLss = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("lod_snow_sensitivity_tooltip"), "How eagerly a distant LOD texture pixel counts as snow. The scale was widened: the old best-at-1.0 now sits near 0.5. Low = only bright white; high = pale gray rock starts counting too. Check with the Terrain Data Provenance debug view (brown = bare, blue-white = snow); the same setting drives the Horizon Snow recolor."));
			ImGui::Checkbox(T(TKEY("lod_replace_legacy"), "Legacy Horizon Shading"), &settings.LODReplaceLegacy);
			if (auto _ttLrl = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("lod_replace_legacy_tooltip"), "A/B comparison: shade horizon snow with the old recolor (vanilla LOD lighting math) instead of the snow shell's own recipe. The old math reads brighter and bluer than the shell, peaking at golden hour. Leave off unless comparing."));
			ImGui::TextDisabled("%s", T(TKEY("distant_snow_fallback_label"), "Fallback snow line (used only where LOD textures are missing):"));

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

			ImGui::Checkbox(T(TKEY("skin_merged_lod_atlases"), "Snow on Merged Distant Objects"), &settings.SkinMergedLODAtlases);
			if (auto _ttAtlas = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("skin_merged_lod_atlases_tooltip"), "DynDOLOD merges many distant objects into single batches wearing a shared atlas texture, whose name gives no clue whether any object in it is snowy. Off, those batches are skipped and the objects inside them carry no distant snow. On, they are covered anyway - measured at +53 objects for +0.05 ms. A merged batch is one mesh, so this is all-or-nothing per batch: turn it off if you ever see snow land on something that should stay bare, such as a shipwreck hull."));

			ImGui::SliderFloat(T(TKEY("skin_distant_bareness"), "Distant Bare Rock"), &settings.SkinDistantBareness, 0.0f, 1.0f, "%.2f");
			if (auto _ttSdb = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("skin_distant_bareness_tooltip"), "How much bare rock distant cliffs and boulders keep. Close up, snow coverage follows the smoothed mesh normal, which on low-poly rocks reports steep flanks as up-facing; near the camera the edge taper hides that, but at range it turns a rock into a white blob. This hands the coverage test over to each face's true orientation as the object shrinks, so steep faces shed their snow again. Raise it for more exposed rock; too high and the mesh's own triangles start to read as jagged facets and seams. 0 keeps the old behaviour."));

			ImGui::TreePop();
		}
		ImGui::PopID();

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

		ImGui::PushID("snow_refill");
		if (ImGui::TreeNodeEx(T(TKEY("menu_advanced"), "Advanced"))) {
			ImGui::SliderFloat(T(TKEY("mound_steepness"), "Mound Steepness"), &settings.SnowMoundSteepness, 0.5f, 3.0f, "%.1f");
			if (auto _ttSteep = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("mound_steepness_tooltip"), "Angle of repose for snow mounds (1.0 = 45 degrees). Steeper = raised snow clings tighter: narrow banks instead of broad aprons, juttier mounds."));

			ImGui::TreePop();
		}
		ImGui::PopID();

		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("undulation"), "Surface Undulation"), ImGuiTreeNodeFlags_Framed)) {
		if (auto _ttUnd = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("undulation_tooltip"), "Wind-worked waves in deep snow. They fade out automatically over thin cover, class borders and carved trench floors."));
		ImGui::SliderFloat(T(TKEY("undulation_strength"), "Undulation Strength"), &settings.UndulationStrength, 0.0f, 8.0f, "%.1f units");
		if (auto _ttUs = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("undulation_strength_tooltip"), "Wave height. 0 flattens deep snow into a smooth sheet."));

		ImGui::SliderFloat(T(TKEY("parallax_depth"), "Parallax Depth"), &settings.ParallaxDepth, 0.0f, 2.0f, "%.2fx");
		if (auto _ttPd = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("parallax_depth_tooltip"), "Parallax occlusion mapping on the landscape shell: marches the view ray through the snow texture's displacement map and shades from where it hits, so grain occludes grain and the surface reads as thick instead of merely lit. Unlike Relief Depth this moves no vertices, and the depth it resolves is by construction the depth of the grain being drawn. A multiplier on the PBR config's displacementScale - 1.0 is exactly the slab depth PBR ground gets. 0 skips the march."));

		ImGui::PushID("undulation");
		if (ImGui::TreeNodeEx(T(TKEY("menu_advanced"), "Advanced"))) {
			ImGui::SliderFloat(T(TKEY("undulation_spacing"), "Undulation Spacing"), &settings.UndulationSpacing, 0.5f, 4.0f, "%.1fx");
			if (auto _ttUsp = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("undulation_spacing_tooltip"), "Stretches the wave pattern: larger = broader, calmer dunes instead of a spike carpet."));

			ImGui::SliderFloat(T(TKEY("relief_depth"), "Relief Depth"), &settings.ReliefDepth, 0.0f, 12.0f, "%.1f units");
			if (auto _ttRd2 = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("relief_depth_tooltip"), "Geometric relief from the snow texture's displacement map on UNTRAMPLED snow, tessellated near the camera. Trenches never receive it (carved ground is excluded), so this costs vertices only on open snowfields: at 0 they stop being subdivided at all, which is most of the Shell pass's tessellation cost, and trench smoothing is unaffected. Note the relief currently samples the displacement map without the anti-tiling offsets the shading uses, so its bumps do not sit where the texture's bumps are."));

			ImGui::SliderInt(T(TKEY("parallax_steps"), "Parallax Steps"), &settings.ParallaxSteps, 4, 16);
			if (auto _ttPs2 = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("parallax_steps_tooltip"), "Coarse steps in the parallax march, before contact refinement re-marches the hit interval at the same budget (so 8 resolves roughly like 64). Scaled down with distance and off entirely past the band where the snow grain stops being drawn. This is a ceiling, not a fixed count - the march exits on first contact, so most pixels never reach it. Measured at Dawnstar, 8 and 16 cost the same to within noise, so raise it freely if you see stepping."));

			ImGui::SliderFloat(T(TKEY("parallax_shadow_strength"), "Parallax Shadow"), &settings.ParallaxShadowStrength, 0.0f, 2.0f, "%.2fx");
			if (auto _ttPss = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("parallax_shadow_strength_tooltip"), "Self-shadowing of the snow's own grain, the same term PBR ground receives from Extended Materials: four taps along the sun through the displacement map, so the micro-relief casts into itself under low sun instead of reading flat. Needs the PBR snow set's _p map. 0 skips the taps entirely (and is the A/B for their cost)."));

			ImGui::TreePop();
		}
		ImGui::PopID();

		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("model_depths"), "Snow Depth by Model Class"), ImGuiTreeNodeFlags_Framed)) {
		if (auto _ttModels = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("model_depths_tooltip"), "Snow layer height per OBJECT model class. Roads are matched by their road/bridge names and textures; flat vs round is classified automatically per mesh."));
		ImGui::SliderFloat(T(TKEY("road_meshes_depth"), "Road Meshes"), &settings.RoadMeshesDepth, 0.0f, 64.0f, "%.0f units");

		ImGui::SliderFloat(T(TKEY("objects_snow_depth"), "Flat Objects"), &settings.ObjectsSnowDepth, 0.0f, 25.0f, "%.0f units");
		if (auto _ttObj = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("objects_snow_depth_tooltip"), "Snow layer on flat hard-edged meshes (walkways, roofs, planks) — these get a completely flat overlay, no fake 3D. Classified automatically per mesh."));

		ImGui::SliderFloat(T(TKEY("snow_meshes_depth"), "Round Objects"), &settings.SnowMeshesDepth, 0.0f, 25.0f, "%.0f units");
		if (auto _ttMesh = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("snow_meshes_depth_tooltip"), "Snow layer on organically smooth meshes (rocks, drifts, logs), where the puffed pillow layer reads correctly in 3D."));

		ImGui::PushID("model_depths");
		if (ImGui::TreeNodeEx(T(TKEY("menu_advanced"), "Advanced"))) {
			ImGui::Checkbox(T(TKEY("object_trenches"), "Trenches on Objects"), &settings.ObjectTrenches);
			if (auto _ttOt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("object_trenches_tooltip"), "Carve footprints into snow sitting on objects (rocks, logs, roofs). Off while the object trenching is being reworked; roads and bridges keep their trenches either way."));
			if (auto _ttRoad = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("road_meshes_depth_tooltip"), "Snow layer on road and bridge meshes. Kept below the surrounding snow classes so the road's course stays readable through the snowfield."));

			ImGui::SliderFloat(T(TKEY("floor_see_through"), "Trench Floor See-Through"), &settings.TrenchFloorFade, 0.0f, 1.0f, "%.2f");
			if (auto _ttFloor = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("floor_see_through_tooltip"), "How much a heavily trampled trench floor on an object (rock, log, walkway) wears through to the object's own surface instead of holding solid snow."));

			ImGui::TreePop();
		}
		ImGui::PopID();

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

		ImGui::SliderFloat(T(TKEY("trench_floor_height"), "Trench Floor Height"), &settings.TrenchFloorHeight, 0.0f, 8.0f, "%.1f units");
		if (auto _ttTfh = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("trench_floor_height_tooltip"), "Minimum snow left on carved trench floors, in units above the terrain. Low values let deep trampling wear through to the real ground, like snow does; 5 restores the old always-solid floors."));

		ImGui::SliderFloat(T(TKEY("workspace_clearing_size"), "Workspace Clearing Size"), &settings.TrampleZoneScale, 0.25f, 2.0f, "%.2fx");
		if (auto _ttWcs = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("workspace_clearing_size_tooltip"), "Radius multiplier for the snow bowls around workstations, smelters, forges, stalls, wells and shrines. Applies within a second."));

		ImGui::SliderFloat(T(TKEY("workspace_clearing_height"), "Workspace Clearing Height"), &settings.TrampleZoneHeight, 0.0f, 100.0f, "%.0f%%");
		if (auto _ttWch = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("workspace_clearing_height_tooltip"), "Snow height remaining in a workspace bowl, as a percent of the surrounding depth. 0 melts to the floor, 100 disables the clearing. Applies within a second."));

		ImGui::SliderFloat(T(TKEY("wall_drift_height"), "Wall Drift Height"), &settings.WallDriftHeight, 0.0f, 48.0f, "%.0f units");
		if (auto _ttWdh = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("wall_drift_height_tooltip"), "Peak height of snow banks drifted against buildings and other large structures. Windward walls bank fully with the weather's wind, calm weather keeps modest banks all around, and the leeward side stays scoured. 0 disables."));

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

			ImGui::SliderFloat(T(TKEY("snow_snow_fade"), "Snow <-> Snow Fade"), &settings.SnowSnowFade, 0.0f, 64.0f, "%.0f units");
			if (auto _ttSs = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("snow_snow_fade_tooltip"), "Cross-fade between OBJECT snow and LANDSCAPE snow where their surfaces run close in height (road meshes, low platforms). Wider = the two snow kinds dither into each other instead of meeting at a hard seam."));

			ImGui::TreePop();
		}
		ImGui::PopID();

		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("trench_detail"), "Landscape Trenches"), ImGuiTreeNodeFlags_Framed)) {
		if (auto _ttTd = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("trench_detail_tooltip"), "The look of disturbed snow: the raised berm along trench edges, the chunky churned surface, and the fine-grain shading detail. Untouched snow is never affected."));
		ImGui::Checkbox(T(TKEY("no_carve_floating"), "Floating Actors Leave No Trench"), &settings.NoCarveFloatingActors);
		if (auto _ttFloat = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("no_carve_floating_tooltip"), "Stops things that never touch the ground from digging it: atronachs, wisps, ghosts, anything that hovers. Nothing is named - an actor is judged by whether its own lowest part ever comes down to its footing, so modded levitators are covered too."));

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
			ImGui::Text("%s", T(TKEY("incorporeal_mode_tooltip"), "Stops things with no substance from digging the snow. This is a separate question from hovering, and has to be: a ghost stands with its feet on the ground like the Nord it otherwise is, so no clearance measurement will ever catch one. See-through bodies judges an actor by whether it is drawn solid, which needs no list and covers modded ghosts; actors marked Ghost reads the flag on the record instead, which never misses a ghost but also catches anything the game made unkillable rather than incorporeal. The Detected line under Spell Integration reports what the nearest actor scores under both."));

		ImGui::Checkbox(T(TKEY("tessellation"), "Tessellate Trenches"), &settings.Tessellation);
		if (auto _ttTess = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("tessellation_tooltip"), "Adds vertex density to the shell and the object trench patch near the camera, keyed off the deformation map, so carves resolve as smooth walls instead of following the coarse grid. This is what trench smoothness actually depends on - Relief Depth only sets how far the extra vertices are then displaced on untrampled snow. Off costs nothing but leaves every trench as angular as the grid beneath it."));

		ImGui::SliderFloat(T(TKEY("berm_height"), "Berm Height"), &settings.BermHeight, 0.0f, 1.0f, "%.2fx");
		if (auto _ttBh = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("berm_height_tooltip"), "Height of the pushed-aside snow ridge along trench edges, as a fraction of the local snow depth. 0 removes the berm."));

		ImGui::PushID("trench_detail");
		if (ImGui::TreeNodeEx(T(TKEY("menu_advanced"), "Advanced"))) {
			ImGui::SliderFloat(T(TKEY("floating_band"), "Floating Actor Clearance"), &settings.FloatingActorBand, 4.0f, 80.0f, "%.0f units");
			if (auto _ttFloatBand = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("floating_band_tooltip"), "How far an actor's lowest part may sit above its footing and still count as standing on it. Lower values catch things that only just hover, at the risk of dropping a normal creature's tracks mid-stride; the Detected line under Spell Integration counts what each value is skipping."));

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
		ImGui::PopID();

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

		ImGui::PushID("obj_trench_detail");
		if (ImGui::TreeNodeEx(T(TKEY("menu_advanced"), "Advanced"))) {
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
				ImGui::Text("             bones: feet %u | limbs %u        (frame totals: feet %u | limbs %u | shapes %u | props %u)",
					stampStats.nearestFeet, stampStats.nearestLimbs,
					stampStats.feet, stampStats.limbs, stampStats.shapes, stampStats.props);
			ImGui::Text("             body alpha %s | marked Ghost %s | verdict %s",
					stampStats.nearestElemental ? "not read" : std::format("{:.2f}", stampStats.nearestBodyAlpha).c_str(),
					stampStats.nearestGhostFlag ? "yes" : "no",
					stampStats.nearestIncorporeal ? "INCORPOREAL" :
						(stampStats.nearestElemental ? "solid (made of an element)" : "solid"));
			}
			ImGui::Text("emitters %u | awaiting their step %u | last mark: strength %.2f radius %.0f",
				spellStats.emitters, spellStats.pending, spellStats.lastStrength, spellStats.lastRadius);
			ImGui::Text("budget: actors+props %u/%u | spells %u/%u | emitters culled by distance %u",
				stampStats.beforeSpells, kMaxStamps - kSpellStampReserve,
				stampStats.spells, kSpellStampReserve, spellStats.emittersCulled);
			ImGui::TreePop();
		}

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
				ImGui::Text("%s", T(TKEY("melt_edge_irregularity_tooltip"), "How far a melted rim wanders off a perfect circle. This moves the outline only and leaves the surface smooth - a melt basin has a wandering edge but no jagged shards, unlike a trampled trail edge, which the separate Trail Irregularity setting churns."));

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

	if (ImGui::TreeNodeEx(T(TKEY("debug_options"), "Debugging Options"), ImGuiTreeNodeFlags_Framed)) {
		ImGui::SeparatorText(T(TKEY("debug_cat_deform_map"), "Deformation Map"));

		ImGui::Checkbox(T(TKEY("show_debug"), "Show Deformation Map"), &settings.ShowDebugTexture);
		if (settings.ShowDebugTexture) {
			ImGui::Text("%s", T(TKEY("debug_hint"), "White = compressed snow. The map follows the camera."));
			ImGui::Image(GetDeformationSRV(), { 512.0f, 512.0f });
		}

		if (ImGui::Button(T(TKEY("clear"), "Clear Deformation Map")))
			clearRequested = true;

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

		ImGui::Checkbox(T(TKEY("debug_overlay"), "Debug Terrain Overlay"), &debugTerrainOverlay);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("debug_overlay_tooltip"), "Paints diagnostics on terrain: red = outside deformation window, green = deformation, blue = detected snow."));

		ImGui::Checkbox(T(TKEY("debug_tiling_ruler"), "Debug Tiling Ruler"), &debugTilingRuler);
		if (auto _ttRuler = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("debug_tiling_ruler_tooltip"), "Measurement aid: draws three gridlines on the landscape. Red = one landscape texture repeat, green = 256 world units (the snow shell's tile), blue = 4096 (cell boundary). Counting red lines per green cell gives the shell-to-landscape tiling ratio directly; the blue lines are the scale anchor. Look straight down at flat ground near the camera."));

		ImGui::SeparatorText(T(TKEY("debug_cat_object_snow"), "Object Snow"));

		{
			const char* staticsDebugModes[] = { "Off", "Edge taper", "Coverage alpha", "Normals" };
			ImGui::Combo(T(TKEY("statics_debug_view"), "Object Snow Debug View"), &staticsDebugView, staticsDebugModes, IM_ARRAYSIZE(staticsDebugModes));
			if (auto _ttSdv = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("statics_debug_view_tooltip"), "Object snow renders its decision data as colors with dithering disabled; missing pixels mean the geometry itself is absent. The trench patch always reads red = trample, green = skin depth. The skins follow the selected mode. Edge taper: red = the height the taper allows, green = up-facing, blue = the raster returned no data. Coverage alpha: red = the opacity the dither sees, green = the facing gates, blue = the seam blends. Normals: red = smoothed normal z (0.5 = horizontal, 1 = straight up), green = the flat/rounded class."));
		}

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
