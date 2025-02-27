/**
 * @file
 */

#include "AbstractVoxFormatTest.h"
#include "io/BufferedReadWriteStream.h"
#include "io/FileStream.h"
#include "voxel/Palette.h"
#include "voxelformat/BinVoxFormat.h"
#include "voxelformat/CubFormat.h"
#include "voxelformat/GLTFFormat.h"
#include "voxelformat/GoxFormat.h"
#include "voxelformat/KV6Format.h"
#include "voxelformat/KVXFormat.h"
#include "voxelformat/OBJFormat.h"
#include "voxelformat/QBCLFormat.h"
#include "voxelformat/QBFormat.h"
#include "voxelformat/QBTFormat.h"
#include "voxelformat/STLFormat.h"
#include "voxelformat/SproxelFormat.h"
#include "voxelformat/VXLFormat.h"
#include "voxelformat/VXMFormat.h"
#include "voxelformat/VXRFormat.h"
#include "voxelformat/VolumeFormat.h"
#include "voxelformat/VoxFormat.h"

namespace voxelformat {

class FormatPaletteTest : public AbstractVoxFormatTest {
protected:
	bool checkNoAlpha(const voxel::Palette &palette) {
		for (int i = 0; i < palette.colorCount(); ++i) {
			if (palette.color(i).a != 255) {
				return false;
			}
		}
		return true;
	}

	// the palettes have to match, as all the colors from the rgb format are saved to the palette of the target format
	void testRGBToPaletteFormat(voxelformat::Format &rgbFormat, const core::String &rgbFile, size_t rgbExpectedColors, voxelformat::Format &paletteFormat, const core::String &palFile, size_t palExpectedColors) {
		io::FileStream rgbStream(open(rgbFile));
		voxel::Palette rgbPalette;
		ASSERT_EQ(rgbExpectedColors, rgbFormat.loadPalette(rgbFile, rgbStream, rgbPalette, testLoadCtx));
		ASSERT_TRUE(checkNoAlpha(rgbPalette));

		rgbStream.seek(0);

		scenegraph::SceneGraph rgbSceneGraph;
		ASSERT_TRUE(rgbFormat.load(rgbFile, rgbStream, rgbSceneGraph, testLoadCtx)) << "Failed to load rgb model " << rgbFile;

		io::BufferedReadWriteStream palWriteStream;
		ASSERT_TRUE(paletteFormat.save(rgbSceneGraph, palFile, palWriteStream, testSaveCtx)) << "Failed to write pal model " << palFile;
		palWriteStream.seek(0);

		voxel::Palette palPalette;
		ASSERT_EQ(paletteFormat.loadPalette(palFile, palWriteStream, palPalette, testLoadCtx), palExpectedColors);
		//ASSERT_TRUE(checkNoAlpha(palPalette));

		for (size_t i = 0; i < rgbExpectedColors; ++i) {
			ASSERT_EQ(rgbPalette.color(i), palPalette.color(i))
				<< i << ": rgb " << core::Color::print(rgbPalette.color(i)) << " versus pal "
				<< core::Color::print(palPalette.color(i)) << "\n"
				<< voxel::Palette::print(rgbPalette) << "\n"
				<< voxel::Palette::print(palPalette);
		}
	}

	// the colors have to match but can differ in their count - the rgb format only saves those colors that are used by at least one voxel
	void testPaletteToRGBFormat(voxelformat::Format &palFormat, const core::String &palFile, size_t palExpectedColors, voxelformat::Format &rgbFormat, const core::String &rgbFile, size_t rgbExpectedColors) {
		io::FileStream palStream(open(palFile));
		voxel::Palette palPalette;
		ASSERT_EQ(palExpectedColors, palFormat.loadPalette(palFile, palStream, palPalette, testLoadCtx));
		//ASSERT_TRUE(checkNoAlpha(palPalette));

		palStream.seek(0);

		scenegraph::SceneGraph palSceneGraph;
		ASSERT_TRUE(palFormat.load(palFile, palStream, palSceneGraph, testLoadCtx)) << "Failed to load pal model " << palFile;

		io::BufferedReadWriteStream rgbWriteStream;
		ASSERT_TRUE(rgbFormat.save(palSceneGraph, rgbFile, rgbWriteStream, testSaveCtx)) << "Failed to write rgb model " << rgbFile;
		rgbWriteStream.seek(0);

		voxel::Palette rgbPalette;
		ASSERT_EQ(rgbFormat.loadPalette(rgbFile, rgbWriteStream, rgbPalette, testLoadCtx), rgbExpectedColors);
		ASSERT_TRUE(checkNoAlpha(rgbPalette));

		for (size_t i = 0; i < rgbExpectedColors; ++i) {
			ASSERT_TRUE(palPalette.hasColor(rgbPalette.color(i)))
				<< i << ": Could not find color " << core::Color::print(rgbPalette.color(i)) << " in pal palette\n"
				<< voxel::Palette::print(palPalette);
		}
	}

	void testRGBToRGBFormat(voxelformat::Format &rgbFormat1, const core::String &rgbFile1, voxelformat::Format &rgbFormat2, const core::String &rgbFile2, size_t expectedColors) {
		io::FileStream palStream(open(rgbFile1));
		voxel::Palette rgbPalette1;
		ASSERT_EQ(expectedColors, rgbFormat1.loadPalette(rgbFile1, palStream, rgbPalette1, testLoadCtx));
		ASSERT_TRUE(checkNoAlpha(rgbPalette1));

		palStream.seek(0);

		scenegraph::SceneGraph palSceneGraph;
		ASSERT_TRUE(rgbFormat1.load(rgbFile1, palStream, palSceneGraph, testLoadCtx)) << "Failed to load rgb model " << rgbFile1;

		io::BufferedReadWriteStream rgbWriteStream;
		ASSERT_TRUE(rgbFormat2.save(palSceneGraph, rgbFile2, rgbWriteStream, testSaveCtx)) << "Failed to write rgb model " << rgbFile2;
		rgbWriteStream.seek(0);

		voxel::Palette rgbPalette2;
		ASSERT_EQ(rgbFormat2.loadPalette(rgbFile2, rgbWriteStream, rgbPalette2, testLoadCtx), expectedColors);
		ASSERT_TRUE(checkNoAlpha(rgbPalette2));

		// the colors might have a different ordering here it depends on the order we read the volume for the rgb format
		for (size_t i = 0; i < expectedColors; ++i) {
			ASSERT_TRUE(rgbPalette1.hasColor(rgbPalette2.color(i)))
				<< i << ": Could not find color " << core::Color::print(rgbPalette2.color(i)) << " in rgb palette\n"
				<< voxel::Palette::print(rgbPalette1);
		}
	}

	void testPaletteToPaletteFormat(voxelformat::Format &palFormat1, const core::String &palFile1, voxelformat::Format &palFormat2, const core::String &palFile2, size_t expectedColors) {
		io::FileStream palStream(open(palFile1));
		voxel::Palette palPalette1;
		ASSERT_EQ(expectedColors, palFormat1.loadPalette(palFile1, palStream, palPalette1, testLoadCtx));
		//ASSERT_TRUE(checkNoAlpha(palPalette1));

		palStream.seek(0);

		scenegraph::SceneGraph palSceneGraph;
		ASSERT_TRUE(palFormat1.load(palFile1, palStream, palSceneGraph, testLoadCtx)) << "Failed to load pal model " << palFile1;

		io::BufferedReadWriteStream rgbWriteStream;
		ASSERT_TRUE(palFormat2.save(palSceneGraph, palFile2, rgbWriteStream, testSaveCtx)) << "Failed to write pal model " << palFile2;
		rgbWriteStream.seek(0);

		voxel::Palette palPalette2;
		ASSERT_EQ(palFormat2.loadPalette(palFile2, rgbWriteStream, palPalette2, testLoadCtx), expectedColors);
		//ASSERT_TRUE(checkNoAlpha(palPalette2));

		for (size_t i = 0; i < expectedColors; ++i) {
			ASSERT_EQ(palPalette1.color(i), palPalette2.color(i))
				<< i << ": pal " << core::Color::print(palPalette1.color(i)) << " versus pal "
				<< core::Color::print(palPalette2.color(i)) << "\n"
				<< voxel::Palette::print(palPalette1) << "\n"
				<< voxel::Palette::print(palPalette2);
		}
	}
};

TEST_F(FormatPaletteTest, testQbToVox) {
	QBFormat rgb;
	VoxFormat pal;
	testRGBToPaletteFormat(rgb, "chr_knight.qb", 17, pal, "chr_knight-qbtovox.vox", 17);
}

TEST_F(FormatPaletteTest, testQbToQb) {
	QBFormat rgb1;
	QBFormat rgb2;
	testRGBToRGBFormat(rgb1, "chr_knight.qb", rgb2, "chr_knight-testqbtoqb.qb", 17);
}

TEST_F(FormatPaletteTest, testQbToQBCL) {
	QBFormat rgb1;
	QBCLFormat rgb2;
	testRGBToRGBFormat(rgb1, "chr_knight.qb", rgb2, "chr_knight-testqbtoqb.qbcl", 17);
}

TEST_F(FormatPaletteTest, testVoxToVox) {
	VoxFormat pal1;
	VoxFormat pal2;
	testPaletteToPaletteFormat(pal1, "magicavoxel.vox", pal2, "magicavoxel-testvoxtovox.qb", 255);
}

TEST_F(FormatPaletteTest, testVoxToQb) {
	QBFormat rgb;
	VoxFormat pal;
	testPaletteToRGBFormat(pal, "magicavoxel.vox", 255, rgb, "magicavoxel-testvoxtoqb.qb", 21);
}

} // namespace voxelformat
