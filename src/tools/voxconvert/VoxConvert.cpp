/**
 * @file
 */

#include "VoxConvert.h"
#include "core/Color.h"
#include "core/Enum.h"
#include "core/GameConfig.h"
#include "core/Log.h"
#include "core/StringUtil.h"
#include "core/Var.h"
#include "command/Command.h"
#include "core/collection/DynamicArray.h"
#include "core/collection/Set.h"
#include "core/concurrent/Concurrency.h"
#include "image/Image.h"
#include "io/FileStream.h"
#include "io/Filesystem.h"
#include "core/TimeProvider.h"
#include "io/FormatDescription.h"
#include "voxel/MaterialColor.h"
#include "voxel/Palette.h"
#include "voxel/PaletteLookup.h"
#include "voxel/RawVolume.h"
#include "voxel/RawVolumeWrapper.h"
#include "voxel/Region.h"
#include "voxelformat/FormatConfig.h"
#include "scenegraph/SceneGraphNode.h"
#include "scenegraph/SceneGraphUtil.h"
#include "voxelformat/VolumeFormat.h"
#include "voxelformat/Format.h"
#include "scenegraph/SceneGraph.h"
#include "voxelgenerator/LUAGenerator.h"
#include "voxelutil/ImageUtils.h"
#include "voxelutil/VolumeCropper.h"
#include "voxelutil/VolumeRescaler.h"
#include "voxelutil/VolumeResizer.h"
#include "voxelutil/VolumeRotator.h"
#include "voxelutil/VolumeSplitter.h"
#include "voxelutil/VolumeVisitor.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>

#define MaxHeightmapWidth 4096
#define MaxHeightmapHeight 4096

VoxConvert::VoxConvert(const io::FilesystemPtr& filesystem, const core::TimeProviderPtr& timeProvider) :
		Super(filesystem, timeProvider, core::cpus()) {
	init(ORGANISATION, "voxconvert");
}

app::AppState VoxConvert::onConstruct() {
	const app::AppState state = Super::onConstruct();
	registerArg("--crop").setDescription("Reduce the volumes to their real voxel sizes");
	registerArg("--dump").setDescription("Dump the scene graph of the input file");
	registerArg("--export-layers").setDescription("Export all the layers of a scene into single files");
	registerArg("--export-palette").setDescription("Export the used palette data into an image");
	registerArg("--filter").setDescription("Layer filter. For example '1-4,6'");
	registerArg("--force").setShort("-f").setDescription("Overwrite existing files");
	registerArg("--image-as-plane").setDescription("Import given input images as planes");
	registerArg("--image-as-volume").setDescription("Import given input image as volume");
	registerArg("--image-as-volume-max-depth").setDefaultValue("8").setDescription("Importing image as volume max depth");
	registerArg("--image-as-volume-both-sides").setDefaultValue("false").setDescription("Importing image as volume for both sides");
	registerArg("--image-as-heightmap").setDescription("Import given input images as heightmaps");
	registerArg("--colored-heightmap").setDescription("Use the alpha channel of the heightmap as height and the rgb data as surface color");
	registerArg("--input").setShort("-i").setDescription("Allow to specify input files");
	registerArg("--merge").setShort("-m").setDescription("Merge layers into one volume");
	registerArg("--mirror").setDescription("Mirror by the given axis (x, y or z)");
	registerArg("--output").setShort("-o").setDescription("Allow to specify the output file");
	registerArg("--rotate").setDescription("Rotate by 90 degree at the given axis (x, y or z), specify e.g. x:180 to rotate around x by 180 degree.");
	registerArg("--resize").setDescription("Resize the volume by the given x (right), y (up) and z (back) values");
	registerArg("--scale").setShort("-s").setDescription("Scale layer to 50% of its original size");
	registerArg("--script").setDefaultValue("script.lua").setDescription("Apply the given lua script to the output volume");
	registerArg("--scriptcolor").setDefaultValue("1").setDescription("Set the palette index that is given to the script parameters");
	registerArg("--split").setDescription("Slices the volumes into pieces of the given size <x:y:z>");
	registerArg("--translate").setShort("-t").setDescription("Translate the volumes by x (right), y (up), z (back)");

	voxelformat::FormatConfig::init();

	_mergeQuads = core::Var::getSafe(cfg::VoxformatMergequads);
	_reuseVertices = core::Var::getSafe(cfg::VoxformatReusevertices);
	_ambientOcclusion = core::Var::getSafe(cfg::VoxformatAmbientocclusion);
	_scale = core::Var::getSafe(cfg::VoxformatScale);
	_scaleX = core::Var::getSafe(cfg::VoxformatScaleX);
	_scaleY = core::Var::getSafe(cfg::VoxformatScaleY);
	_scaleZ = core::Var::getSafe(cfg::VoxformatScaleZ);
	_quads = core::Var::getSafe(cfg::VoxformatQuads);
	_withColor = core::Var::getSafe(cfg::VoxformatWithcolor);
	_withTexCoords = core::Var::getSafe(cfg::VoxformatWithtexcoords);

	if (!filesystem()->registerPath("scripts/")) {
		Log::warn("Failed to register lua generator script path");
	}

	return state;
}

void VoxConvert::usage() const {
	Super::usage();
	Log::info("Load support:");
	for (const io::FormatDescription *desc = voxelformat::voxelLoad(); desc->valid(); ++desc) {
		for (const core::String& ext : desc->exts) {
			Log::info(" * %s (*.%s)", desc->name.c_str(), ext.c_str());
		}
	}
	Log::info("Save support:");
	for (const io::FormatDescription *desc = voxelformat::voxelSave(); desc->valid(); ++desc) {
		for (const core::String& ext : desc->exts) {
			Log::info(" * %s (*.%s)", desc->name.c_str(), ext.c_str());
		}
	}
	Log::info("Supported image formats:");
	for (const io::FormatDescription *desc = io::format::images(); desc->valid(); ++desc) {
		for (const core::String& ext : desc->exts) {
			Log::info(" * %s (*.%s)", desc->name.c_str(), ext.c_str());
		}
	}
	Log::info("Supported palette formats:");
	for (const io::FormatDescription *desc = io::format::palettes(); desc->valid(); ++desc) {
		for (const core::String& ext : desc->exts) {
			Log::info(" * %s (*.%s)", desc->name.c_str(), ext.c_str());
		}
	}
	Log::info("Built-in palettes:");
	for (int i = 0; i < lengthof(voxel::Palette::builtIn); ++i) {
		Log::info(" * %s", voxel::Palette::builtIn[i]);
	}
	Log::info("Links:");
	Log::info(" * Bug reports: https://github.com/mgerhardy/vengi");
	Log::info(" * Twitter: https://twitter.com/MartinGerhardy");
	Log::info(" * Mastodon: https://mastodon.social/@mgerhardy");
	Log::info(" * Discord: https://discord.gg/AgjCPXy");
}

app::AppState VoxConvert::onInit() {
	const app::AppState state = Super::onInit();
	if (state != app::AppState::Running) {
		return state;
	}

	if (_argc < 2) {
		_logLevelVar->setVal(SDL_LOG_PRIORITY_INFO);
		Log::init();
		usage();
		return app::AppState::InitFailure;
	}

	const bool hasScript = hasArg("--script");

	core::String infilesstr;
	core::DynamicArray<core::String> infiles;
	bool inputIsMesh = false;
	if (hasArg("--input")) {
		int argn = 0;
		for (;;) {
			core::String val = getArgVal("--input", "", &argn);
			if (val.empty()) {
				break;
			}
			io::normalizePath(val);
			infiles.push_back(val);
			if (voxelformat::isMeshFormat(val)) {
				inputIsMesh = true;
			}
			if (!infilesstr.empty()) {
				infilesstr += ", ";
			}
			infilesstr += val;
		}
	} else if (!hasScript) {
		Log::error("No input file was specified");
		return app::AppState::InitFailure;
	}
	core::String outfile;
	if (hasArg("--output")) {
		outfile = getArgVal("--output");
		io::normalizePath(outfile);
	}

	_mergeVolumes     = hasArg("--merge");
	_scaleVolumes     = hasArg("--scale");
	_mirrorVolumes    = hasArg("--mirror");
	_rotateVolumes    = hasArg("--rotate");
	_translateVolumes = hasArg("--translate");
	_exportPalette    = hasArg("--export-palette");
	_exportLayers     = hasArg("--export-layers");
	_cropVolumes      = hasArg("--crop");
	_splitVolumes     = hasArg("--split");
	_dumpSceneGraph   = hasArg("--dump");
	_resizeVolumes    = hasArg("--resize");

	Log::info("Options");
	if (inputIsMesh || voxelformat::isMeshFormat(outfile)) {
		Log::info("* mergeQuads:        - %s", _mergeQuads->strVal().c_str());
		Log::info("* reuseVertices:     - %s", _reuseVertices->strVal().c_str());
		Log::info("* ambientOcclusion:  - %s", _ambientOcclusion->strVal().c_str());
		Log::info("* scale:             - %s", _scale->strVal().c_str());
		Log::info("* scaleX:            - %s", _scaleX->strVal().c_str());
		Log::info("* scaleY:            - %s", _scaleY->strVal().c_str());
		Log::info("* scaleZ:            - %s", _scaleZ->strVal().c_str());
		Log::info("* quads:             - %s", _quads->strVal().c_str());
		Log::info("* withColor:         - %s", _withColor->strVal().c_str());
		Log::info("* withTexCoords:     - %s", _withTexCoords->strVal().c_str());
	}
	const core::VarPtr &paletteVar = core::Var::getSafe(cfg::VoxelPalette);
	if (!paletteVar->strVal().empty()) {
		Log::info("* palette:           - %s", paletteVar->strVal().c_str());
	}
	Log::info("* input files:       - %s", infilesstr.c_str());
	if (!outfile.empty()) {
		Log::info("* output files:      - %s", outfile.c_str());
	}

	if (io::isA(outfile, io::format::palettes()) && infiles.size() == 1) {
		voxel::Palette palette;
		if (!voxelformat::importPalette(infiles[0], palette)) {
			Log::error("Failed to import the palette from %s", infiles[0].c_str());
			return app::AppState::InitFailure;
		}
		if (palette.save(outfile.c_str())) {
			Log::info("Saved palette with %i colors to %s", palette.colorCount(), outfile.c_str());
			return state;
		}
		Log::error("Failed to write %s", outfile.c_str());
		return app::AppState::InitFailure;
	}

	core::String scriptParameters;
	if (hasScript) {
		scriptParameters = getArgVal("--script");
		if (scriptParameters.empty()) {
			Log::error("Missing script parameters");
		}
		Log::info("* script:            - %s", scriptParameters.c_str());
	}
	Log::info("* dump scene graph:  - %s", (_dumpSceneGraph   ? "true" : "false"));
	Log::info("* merge volumes:     - %s", (_mergeVolumes     ? "true" : "false"));
	Log::info("* scale volumes:     - %s", (_scaleVolumes     ? "true" : "false"));
	Log::info("* crop volumes:      - %s", (_cropVolumes      ? "true" : "false"));
	Log::info("* split volumes:     - %s", (_splitVolumes     ? "true" : "false"));
	Log::info("* mirror volumes:    - %s", (_mirrorVolumes    ? "true" : "false"));
	Log::info("* translate volumes: - %s", (_translateVolumes ? "true" : "false"));
	Log::info("* rotate volumes:    - %s", (_rotateVolumes    ? "true" : "false"));
	Log::info("* export palette:    - %s", (_exportPalette    ? "true" : "false"));
	Log::info("* export layers:     - %s", (_exportLayers     ? "true" : "false"));
	Log::info("* resize volumes:    - %s", (_resizeVolumes    ? "true" : "false"));

	voxel::Palette palette = voxel::getPalette();

	io::FilePtr outputFile;
	if (!outfile.empty()) {
		const bool outfileExists = filesystem()->open(outfile)->exists();
		if (outfileExists) {
			if (!hasArg("--force")) {
				Log::error("Given output file '%s' already exists", outfile.c_str());
				return app::AppState::InitFailure;
			}
		}

		outputFile = filesystem()->open(outfile, io::FileMode::SysWrite);
		if (!outputFile->validHandle()) {
			Log::error("Could not open target file: %s", outfile.c_str());
			return app::AppState::InitFailure;
		}
	} else if (!_exportLayers && !_exportPalette && !_dumpSceneGraph) {
		Log::error("No output specified");
		return app::AppState::InitFailure;
	}

	scenegraph::SceneGraph sceneGraph;
	for (const core::String& infile : infiles) {
		if (filesystem()->isReadableDir(infile)) {
			core::DynamicArray<io::FilesystemEntry> entities;
			filesystem()->list(infile, entities);
			Log::info("Found %i entries in dir %s", (int)entities.size(), infile.c_str());
			int success = 0;
			for (const io::FilesystemEntry &entry : entities) {
				if (entry.type != io::FilesystemEntry::Type::file) {
					continue;
				}
				const core::String fullpath = core::string::path(infile, entry.name);
				if (!handleInputFile(fullpath, sceneGraph, infiles.size() > 1)) {
					++success;
				}
			}
			if (success == 0) {
				Log::error("Could not find a valid input file in directory %s", infile.c_str());
				return app::AppState::InitFailure;
			}
		} else {
			if (!handleInputFile(infile, sceneGraph, infiles.size() > 1)) {
				return app::AppState::InitFailure;
			}
		}
	}
	if (!scriptParameters.empty() && sceneGraph.empty()) {
		scenegraph::SceneGraphNode node(scenegraph::SceneGraphNodeType::Model);
		const voxel::Region region(0, 63);
		node.setVolume(new voxel::RawVolume(region), true);
		node.setName("Script generated");
		sceneGraph.emplace(core::move(node));
	}

	if (sceneGraph.empty()) {
		Log::error("No valid input found in the scenegraph to operate on.");
		return app::AppState::InitFailure;
	}

	const bool applyFilter = hasArg("--filter");
	if (applyFilter) {
		if (infiles.size() == 1u) {
			filterVolumes(sceneGraph);
		} else {
			Log::warn("Don't apply layer filters for multiple input files");
		}
	}

	if (_exportLayers) {
		if (infiles.size() > 1) {
			Log::warn("The format and path of the first input file is used for exporting all layers");
		}
		exportLayersIntoSingleObjects(sceneGraph, infiles[0]);
	}

	if (_mergeVolumes) {
		Log::info("Merge layers");
		const scenegraph::SceneGraph::MergedVolumePalette &merged = sceneGraph.merge();
		if (merged.first == nullptr) {
			Log::error("Failed to merge volumes");
			return app::AppState::InitFailure;
		}
		sceneGraph.clear();
		scenegraph::SceneGraphNode node;
		node.setPalette(merged.second);
		node.setVolume(merged.first, true);
		node.setName(infilesstr);
		sceneGraph.emplace(core::move(node));
	}

	if (_scaleVolumes) {
		scale(sceneGraph);
	}

	if (_resizeVolumes) {
		resize(getArgIvec3("--resize"), sceneGraph);
	}

	if (_mirrorVolumes) {
		mirror(getArgVal("--mirror"), sceneGraph);
	}

	if (_rotateVolumes) {
		rotate(getArgVal("--rotate"), sceneGraph);
	}

	if (_translateVolumes) {
		translate(getArgIvec3("--translate"), sceneGraph);
	}

	if (!scriptParameters.empty()) {
		const core::String &color = getArgVal("--scriptcolor");
		script(scriptParameters, sceneGraph, color.toInt());
	}

	if (_cropVolumes) {
		crop(sceneGraph);
	}

	if (_splitVolumes) {
		split(getArgIvec3("--split"), sceneGraph);
	}

	if (outputFile) {
		Log::debug("Save %i volumes", (int)sceneGraph.size());
		voxelformat::SaveContext saveCtx;
		if (!voxelformat::saveFormat(outputFile, nullptr, sceneGraph, saveCtx)) {
			Log::error("Failed to write to output file '%s'", outfile.c_str());
			return app::AppState::InitFailure;
		}
		Log::info("Wrote output file %s", outputFile->name().c_str());
	}
	return state;
}

core::String VoxConvert::getFilenameForLayerName(const core::String &inputfile, const core::String &layerName, int id) {
	const core::String &ext = core::string::extractExtension(inputfile);
	core::String name;
	if (layerName.empty()) {
		name = core::string::format("layer-%i.%s", id, ext.c_str());
	} else {
		name = core::string::format("%s.%s", layerName.c_str(), ext.c_str());
	}
	return core::string::path(core::string::extractPath(inputfile), core::string::sanitizeFilename(name));
}

static void printProgress(const char *name, int cur, int max) {
	// Log::info("%s: %i/%i", name, cur, max);
}

bool VoxConvert::handleInputFile(const core::String &infile, scenegraph::SceneGraph &sceneGraph, bool multipleInputs) {
	Log::info("-- current input file: %s", infile.c_str());
	const io::FilePtr inputFile = filesystem()->open(infile, io::FileMode::SysRead);
	if (!inputFile->exists()) {
		Log::error("Given input file '%s' does not exist", infile.c_str());
		_exitCode = 127;
		return false;
	}
	const bool inputIsImage = inputFile->isAnyOf(io::format::images());
	if (inputIsImage) {
		const image::ImagePtr& image = image::loadImage(inputFile);
		if (!image || !image->isLoaded()) {
			Log::error("Couldn't load image %s", infile.c_str());
			return false;
		}
		const bool importAsPlane = hasArg("--image-as-plane");
		const bool importAsVolume = hasArg("--image-as-volume");
		const bool importAsHeightmap = hasArg("--image-as-heightmap");
		if (importAsHeightmap || (!importAsPlane && !importAsVolume && !_exportPalette)) {
			const bool coloredHeightmap = hasArg("--colored-heightmap");
			if (image->width() > MaxHeightmapWidth || image->height() >= MaxHeightmapHeight) {
				Log::warn("Skip creating heightmap - image dimensions exceeds the max allowed boundaries");
				return false;
			}
			const int maxHeight = voxelutil::importHeightMaxHeight(image, coloredHeightmap);
			if (maxHeight == 0) {
				Log::error("There is no height in either the red channel or the alpha channel");
				return false;
			}
			if (maxHeight == 1) {
				Log::warn("There is no height value in the image - it is imported as flat plane");
			}
			Log::info("Generate from heightmap (%i:%i) with max height of %i", image->width(), image->height(), maxHeight);
			voxel::Region region(0, 0, 0, image->width(), maxHeight - 1, image->height());
			voxel::RawVolume* volume = new voxel::RawVolume(region);
			voxel::RawVolumeWrapper wrapper(volume);
			scenegraph::SceneGraphNode node(scenegraph::SceneGraphNodeType::Model);
			const voxel::Voxel dirtVoxel = voxel::createVoxel(voxel::VoxelType::Generic, 1);
			if (coloredHeightmap) {
				voxel::PaletteLookup palLookup;
				voxelutil::importColoredHeightmap(wrapper, palLookup, image, dirtVoxel);
				node.setPalette(palLookup.palette());
			} else {
				const voxel::Voxel grassVoxel = voxel::createVoxel(voxel::VoxelType::Generic, 2);
				voxelutil::importHeightmap(wrapper, image, dirtVoxel, grassVoxel);
			}
			node.setVolume(volume, true);
			node.setName(core::string::extractFilename(infile));
			sceneGraph.emplace(core::move(node));
		}
		if (importAsVolume) {
			scenegraph::SceneGraphNode node;
			const int maxDepth = glm::clamp(core::string::toInt(getArgVal("--image-as-volume-max-depth")), 1, 255);
			const bool bothSides = core::string::toBool(getArgVal("--image-as-volume-both-sides"));
			node.setVolume(voxelutil::importAsVolume(image, maxDepth, bothSides), true);
			node.setName(core::string::extractFilename(infile));
			sceneGraph.emplace(core::move(node));
		}
		if (importAsPlane) {
			scenegraph::SceneGraphNode node;
			node.setVolume(voxelutil::importAsPlane(image), true);
			node.setName(core::string::extractFilename(infile));
			sceneGraph.emplace(core::move(node));
		}
		if (_exportPalette) {
			voxel::Palette pal;
			const core::String &filename = core::string::replaceExtension(infile, "png");
			pal.convertImageToPalettePng(image, filename.c_str());
		}
	} else {
		io::FileStream inputFileStream(inputFile);
		scenegraph::SceneGraph newSceneGraph;
		voxelformat::LoadContext loadCtx;
		loadCtx.monitor = printProgress;
		if (!voxelformat::loadFormat(inputFile->name(), inputFileStream, newSceneGraph, loadCtx)) {
			return false;
		}

		int parent = sceneGraph.root().id();
		if (multipleInputs) {
			scenegraph::SceneGraphNode groupNode(scenegraph::SceneGraphNodeType::Group);
			groupNode.setName(core::string::extractFilename(infile));
			parent = sceneGraph.emplace(core::move(groupNode), parent);
		}
		scenegraph::addSceneGraphNodes(sceneGraph, newSceneGraph, parent);
		if (_dumpSceneGraph) {
			dump(sceneGraph);
		}

		if (_exportPalette) {
			const core::String &paletteFile = core::string::replaceExtension(infile, "png");
			sceneGraph.firstPalette().save(paletteFile.c_str());
		}
	}

	return true;
}

void VoxConvert::exportLayersIntoSingleObjects(scenegraph::SceneGraph& sceneGraph, const core::String &inputfile) {
	Log::info("Export layers into single objects");
	int n = 0;
	voxelformat::SaveContext saveCtx;
	for (scenegraph::SceneGraphNode& node : sceneGraph) {
		scenegraph::SceneGraph newSceneGraph;
		scenegraph::SceneGraphNode newNode;
		scenegraph::copyNode(node, newNode, false);
		newSceneGraph.emplace(core::move(newNode));
		const core::String& filename = getFilenameForLayerName(inputfile, node.name(), n);
		if (voxelformat::saveFormat(filesystem()->open(filename, io::FileMode::SysWrite), nullptr, newSceneGraph, saveCtx)) {
			Log::info(" .. %s", filename.c_str());
		} else {
			Log::error(" .. %s", filename.c_str());
		}
	}
}

glm::ivec3 VoxConvert::getArgIvec3(const core::String &name) {
	const core::String &arguments = getArgVal(name);
	glm::ivec3 t(0);
	SDL_sscanf(arguments.c_str(), "%i:%i:%i", &t.x, &t.y, &t.z);
	return t;
}

void VoxConvert::split(const glm::ivec3 &size, scenegraph::SceneGraph& sceneGraph) {
	Log::info("split volumes at %i:%i:%i", size.x, size.y, size.z);
	const scenegraph::SceneGraph::MergedVolumePalette &merged = sceneGraph.merge();
	sceneGraph.clear();
	core::DynamicArray<voxel::RawVolume *> rawVolumes;
	voxelutil::splitVolume(merged.first, size, rawVolumes);
	delete merged.first;
	for (voxel::RawVolume *v : rawVolumes) {
		scenegraph::SceneGraphNode node;
		node.setVolume(v, true);
		node.setPalette(merged.second);
		sceneGraph.emplace(core::move(node));
	}
}

int VoxConvert::dumpNode_r(const scenegraph::SceneGraph& sceneGraph, int nodeId, int indent) {
	const scenegraph::SceneGraphNode& node = sceneGraph.node(nodeId);

	const scenegraph::SceneGraphNodeType type = node.type();

	Log::info("%*sNode: %i (parent %i)", indent, " ", nodeId, node.parent());
	Log::info("%*s  |- name: %s", indent, " ", node.name().c_str());
	Log::info("%*s  |- type: %s", indent, " ", scenegraph::SceneGraphNodeTypeStr[core::enumVal(type)]);
	const glm::vec3 &pivot = node.pivot();
	Log::info("%*s  |- pivot %f:%f:%f", indent, " ", pivot.x, pivot.y, pivot.z);
	int voxels = 0;
	if (type == scenegraph::SceneGraphNodeType::Model) {
		voxel::RawVolume *v = node.volume();
		Log::info("%*s  |- volume: %s", indent, " ", v != nullptr ? v->region().toString().c_str() : "no volume");
		if (v) {
			voxelutil::visitVolume(*v, [&](int, int, int, const voxel::Voxel &) { ++voxels; });
		}
		Log::info("%*s  |- voxels: %i", indent, " ", voxels);
	} else if (type == scenegraph::SceneGraphNodeType::Camera) {
		const scenegraph::SceneGraphNodeCamera &cameraNode = scenegraph::toCameraNode(node);
		Log::info("%*s  |- field of view: %i", indent, " ", cameraNode.fieldOfView());
		Log::info("%*s  |- nearplane: %f", indent, " ", cameraNode.nearPlane());
		Log::info("%*s  |- farplane: %f", indent, " ", cameraNode.farPlane());
		Log::info("%*s  |- mode: %s", indent, " ", cameraNode.isOrthographic() ? "ortho" : "perspective");
	}
	for (const auto & entry : node.properties()) {
		Log::info("%*s  |- %s: %s", indent, " ", entry->key.c_str(), entry->value.c_str());
	}
	for (const scenegraph::SceneGraphKeyFrame &kf : node.keyFrames()) {
		Log::info("%*s  |- keyframe: %i", indent, " ", kf.frameIdx);
		Log::info("%*s    |- long rotation: %s", indent, " ", kf.longRotation ? "true" : "false");
		Log::info("%*s    |- interpolation: %s", indent, " ", scenegraph::InterpolationTypeStr[core::enumVal(kf.interpolation)]);
		Log::info("%*s    |- transform", indent, " ");
		const scenegraph::SceneGraphTransform &transform = kf.transform();
		const glm::vec3 &tr  = transform.worldTranslation();
		Log::info("%*s      |- translation %f:%f:%f", indent, " ", tr.x, tr.y, tr.z);
		const glm::vec3 &ltr = transform.localTranslation();
		Log::info("%*s      |- local translation %f:%f:%f", indent, " ", ltr.x, ltr.y, ltr.z);
		const glm::quat &rt = transform.worldOrientation();
		const glm::vec3 &rtEuler = glm::degrees(glm::eulerAngles(rt));
		Log::info("%*s      |- orientation %f:%f:%f:%f", indent, " ", rt.x, rt.y, rt.z, rt.w);
		Log::info("%*s        |- euler %f:%f:%f", indent, " ", rtEuler.x, rtEuler.y, rtEuler.z);
		const glm::quat &lrt = transform.localOrientation();
		const glm::vec3 &lrtEuler = glm::degrees(glm::eulerAngles(lrt));
		Log::info("%*s      |- local orientation %f:%f:%f:%f", indent, " ", lrt.x, lrt.y, lrt.z, lrt.w);
		Log::info("%*s        |- euler %f:%f:%f", indent, " ", lrtEuler.x, lrtEuler.y, lrtEuler.z);
		const glm::vec3 &sc = transform.worldScale();
		Log::info("%*s      |- scale %f:%f:%f", indent, " ", sc.x, sc.y, sc.z);
		const glm::vec3 &lsc = transform.localScale();
		Log::info("%*s      |- local scale %f:%f:%f", indent, " ", lsc.x, lsc.y, lsc.z);
	}
	Log::info("%*s  |- children: %i", indent, " ", (int)node.children().size());
	for (int children : node.children()) {
		voxels += dumpNode_r(sceneGraph, children, indent + 2);
	}
	return voxels;
}

void VoxConvert::dump(const scenegraph::SceneGraph& sceneGraph) {
	int voxels = dumpNode_r(sceneGraph, sceneGraph.root().id(), 0);
	Log::info("Voxel count: %i", voxels);
}

void VoxConvert::crop(scenegraph::SceneGraph& sceneGraph) {
	Log::info("Crop volumes");
	for (scenegraph::SceneGraphNode& node : sceneGraph) {
		node.setVolume(voxelutil::cropVolume(node.volume()), true);
	}
}

void VoxConvert::script(const core::String &scriptParameters, scenegraph::SceneGraph& sceneGraph, uint8_t color) {
	voxelgenerator::LUAGenerator script;
	if (!script.init()) {
		Log::warn("Failed to initialize the script bindings");
	} else {
		core::DynamicArray<core::String> tokens;
		core::string::splitString(scriptParameters, tokens);
		const core::String &luaScript = script.load(tokens[0]);
		if (luaScript.empty()) {
			Log::error("Failed to load %s", tokens[0].c_str());
		} else {
			const voxel::Voxel voxel = voxel::createVoxel(voxel::VoxelType::Generic, color);
			core::DynamicArray<voxelgenerator::LUAParameterDescription> argsInfo;
			if (!script.argumentInfo(luaScript, argsInfo)) {
				Log::warn("Failed to get argument details");
			}
			core::DynamicArray<core::String> args(tokens.size() - 1);
			for (size_t i = 1; i < tokens.size(); ++i) {
				args[i - 1] = tokens[i];
			}
			Log::info("Execute script %s", tokens[0].c_str());
			for (scenegraph::SceneGraphNode& node : sceneGraph) {
				voxel::Region dirtyRegion = voxel::Region::InvalidRegion;
				script.exec(luaScript, sceneGraph, node.id(), node.region(), voxel, dirtyRegion, args);
			}
		}
	}

	script.shutdown();
}

void VoxConvert::scale(scenegraph::SceneGraph& sceneGraph) {
	Log::info("Scale layers");
	for (scenegraph::SceneGraphNode& node : sceneGraph) {
		const voxel::Region srcRegion = node.region();
		const glm::ivec3& targetDimensionsHalf = (srcRegion.getDimensionsInVoxels() / 2) - 1;
		const voxel::Region destRegion(srcRegion.getLowerCorner(), srcRegion.getLowerCorner() + targetDimensionsHalf);
		if (destRegion.isValid()) {
			voxel::RawVolume* destVolume = new voxel::RawVolume(destRegion);
			voxelutil::rescaleVolume(*node.volume(), node.palette(), *destVolume);
			node.setVolume(destVolume, true);
		}
	}
}

void VoxConvert::resize(const glm::ivec3 &size, scenegraph::SceneGraph& sceneGraph) {
	Log::info("Resize layers");
	for (scenegraph::SceneGraphNode& node : sceneGraph) {
		node.setVolume(voxelutil::resize(node.volume(), size), true);
	}
}

void VoxConvert::filterVolumes(scenegraph::SceneGraph& sceneGraph) {
	const core::String &filter = getArgVal("--filter");
	if (filter.empty()) {
		Log::warn("No filter specified");
		return;
	}

	core::Set<int> layers;
	core::DynamicArray<core::String> tokens;
	core::string::splitString(filter, tokens, ",");
	for (const core::String& token : tokens) {
		if (token.contains("-")) {
			const int start = token.toInt();
			const size_t index = token.find("-");
			const core::String &endString = token.substr(index + 1);
			const int end = endString.toInt();
			for (int layer = start; layer <= end; ++layer) {
				layers.insert(layer);
			}
		} else {
			const int layer = token.toInt();
			layers.insert(layer);
		}
	}
	for (int i = 0; i < (int)sceneGraph.size(); ++i) {
		if (!layers.has(i)) {
			sceneGraph[i]->release();
			Log::debug("Remove layer %i - not part of the filter expression", i);
		}
	}
	Log::info("Filtered layers: %i", (int)layers.size());
}

void VoxConvert::mirror(const core::String& axisStr, scenegraph::SceneGraph& sceneGraph) {
	const math::Axis axis = math::toAxis(axisStr);
	if (axis == math::Axis::None) {
		return;
	}
	Log::info("Mirror on axis %c", axisStr[0]);
	for (scenegraph::SceneGraphNode &node : sceneGraph) {
		node.setVolume(voxelutil::mirrorAxis(node.volume(), axis), true);
	}
}

void VoxConvert::rotate(const core::String& axisStr, scenegraph::SceneGraph& sceneGraph) {
	const math::Axis axis = math::toAxis(axisStr);
	if (axis == math::Axis::None) {
		return;
	}
	float degree = 90.0f;
	if (axisStr.contains(":")) {
		degree = glm::mod(axisStr.substr(2).toFloat(), 360.0f);
	}
	if (degree <= 1.0f) {
		Log::warn("Don't rotate onaxis %c by %f degree", axisStr[0], degree);
		return;
	}
	Log::info("Rotate on axis %c by %f degree", axisStr[0], degree);
	for (scenegraph::SceneGraphNode &node : sceneGraph) {
		glm::vec3 rotVec{0.0f};
		rotVec[math::getIndexForAxis(axis)] = degree;
		node.setVolume(voxelutil::rotateVolume(node.volume(), rotVec, glm::vec3(0.5f)), true);
	}
}

void VoxConvert::translate(const glm::ivec3& pos, scenegraph::SceneGraph& sceneGraph) {
	Log::info("Translate by %i:%i:%i", pos.x, pos.y, pos.z);
	for (scenegraph::SceneGraphNode &node : sceneGraph) {
		if (voxel::RawVolume *v = node.volume()) {
			v->translate(pos);
		}
	}
}

int main(int argc, char *argv[]) {
	const io::FilesystemPtr& filesystem = core::make_shared<io::Filesystem>();
	const core::TimeProviderPtr& timeProvider = core::make_shared<core::TimeProvider>();
	VoxConvert app(filesystem, timeProvider);
	return app.startMainLoop(argc, argv);
}
