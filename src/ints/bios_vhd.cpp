/*
*
*  Copyright (c) 2018 Shane Krueger
*
*  This program is free software; you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation; either version 2 of the License, or
*  (at your option) any later version.
*
*  This program is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with this program; if not, write to the Free Software
*  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

#include "dosbox.h"
#include "callback.h"
#include "bios.h"
#include "bios_disk.h"
#include "regs.h"
#include "mem.h"
#include "dos_inc.h" /* for Drives[] */
#include "../dos/drives.h"
#include "mapper.h"
#include "SDL.h"

/*
* imageDiskVHD supports fixed, dynamic, and differential VHD file formats
*
* Public members:  Open(), GetVHDType()
*
* Notes:
* - create instance using Open()
* - code assumes that c/h/s values stored within VHD file are accurate, and does not scan the MBR
* - VHDs with 511 byte footers are not yet supported
* - VHDX files are not supported
* - checksum values are verified; and the backup header is used if the footer cannot be found or has an invalid checksum
* - code does not prevent loading if the VHD is marked as being part of a saved state
* - code prevents loading if parent does not match correct guid
* - code does not prevent loading if parent does not match correct datestamp
*
*/

imageDiskVHD::ErrorCodes imageDiskVHD::Open(const char* fileName, const bool readOnly, imageDisk** disk) {
	return Open(fileName, readOnly, disk, 0);
}

imageDiskVHD::ErrorCodes imageDiskVHD::Open(const char* fileName, const bool readOnly, imageDisk** disk, const Bit8u* matchUniqueId) {
	//verify that C++ didn't add padding to the structure layout
	assert(sizeof(VHDFooter) == 512);
	assert(sizeof(DynamicHeader) == 1024);
	//open file and clear C++ buffering
	FILE* file = fopen64(fileName, readOnly ? "r" : "rb+");
	if (!file) return ERROR_OPENING;
	setbuf(file, NULL);
	//check that length of file is > 512 bytes
	if (fseeko64(file, 0L, SEEK_END)) { fclose(file); return INVALID_DATA; }
	if (ftello64(file) < 512) { fclose(file); return INVALID_DATA; }
	//read footer
	if (fseeko64(file, -512L, SEEK_CUR)) { fclose(file); return INVALID_DATA; }
	Bit64u footerPosition = ftello64(file);
	if (footerPosition == -1) { fclose(file); return INVALID_DATA; }
	VHDFooter originalfooter;
	VHDFooter footer;
	if (fread(&originalfooter, 512, 1, file) != 1) { fclose(file); return INVALID_DATA; }
	//convert from big-endian if necessary
	footer = originalfooter;
	footer.SwapByteOrder();
	//verify checksum on footer
	if (!footer.IsValid()) {
		//if invalid, read header, and verify checksum on header
		if (fseeko64(file, 0L, SEEK_SET)) { fclose(file); return INVALID_DATA; }
		if (fread(&originalfooter, sizeof(Bit8u), 512, file) != 512) { fclose(file); return INVALID_DATA; }
		//convert from big-endian if necessary
		footer = originalfooter;
		footer.SwapByteOrder();
		//verify checksum on header
		if (!footer.IsValid()) { fclose(file); return INVALID_DATA; }
	}
	//check that uniqueId matches
	if (matchUniqueId && memcmp(matchUniqueId, footer.uniqueId, 16)) { fclose(file); return INVALID_MATCH; }
	//calculate disk size
	Bit64u calcDiskSize = (Bit64u)footer.geometry.cylinders * (Bit64u)footer.geometry.heads * (Bit64u)footer.geometry.sectors * (Bit64u)512;
	if (!calcDiskSize) { fclose(file); return INVALID_DATA; }
	//if fixed image, return plain imageDisk rather than imageDiskVFD
	if (footer.diskType == VHD_TYPE_FIXED) {
		//make sure that the image size is at least as big as the geometry
		if (calcDiskSize > footerPosition) {
			fclose(file);
			return INVALID_DATA;
		}
		imageDisk* d = new imageDisk();
		d->cylinders = footer.geometry.cylinders;
		d->heads = footer.geometry.heads;
		d->sectors = footer.geometry.sectors;
		d->sector_size = 512;
		d->diskSizeK = (Bit32u)(calcDiskSize / (Bit64u)1024); //impossible to overflow
		d->diskimg = file;
		d->hardDrive = true;
		d->active = true;
		*disk = d;
		return OPEN_SUCCESS;
	}

	//set up imageDiskVHD here
	imageDiskVHD* vhd = new imageDiskVHD();
	vhd->footerPosition = footerPosition;
	vhd->footer = footer;
	vhd->vhdType = footer.diskType;
	vhd->originalFooter = originalfooter;
	vhd->cylinders = footer.geometry.cylinders;
	vhd->heads = footer.geometry.heads;
	vhd->sectors = footer.geometry.sectors;
	vhd->sector_size = 512;
	vhd->diskSizeK = (Bit32u)(calcDiskSize / (Bit64u)1024); //impossible to overflow
	vhd->diskimg = file;
	vhd->hardDrive = true;
	vhd->active = true;
	//use delete vhd from now on to release the disk image upon failure

	//if not dynamic or differencing, fail
	if (footer.diskType != VHD_TYPE_DYNAMIC && footer.diskType != VHD_TYPE_DIFFERENCING) {
		delete vhd;
		return UNSUPPORTED_TYPE;
	}
	//read dynamic disk header (applicable for dynamic and differencing types)
	DynamicHeader dynHeader;
	if (fseeko64(file, footer.dataOffset, SEEK_SET)) { delete vhd; return INVALID_DATA; }
	if (fread(&dynHeader, sizeof(Bit8u), 1024, file) != 1024) { delete vhd; return INVALID_DATA; }
	//swap byte order and validate checksum
	dynHeader.SwapByteOrder();
	if (!dynHeader.IsValid()) { delete vhd; return INVALID_DATA; }
	//check block size is valid (should be a power of 2, and at least equal to one sector)
	if (dynHeader.blockSize < 512 || (dynHeader.blockSize & (dynHeader.blockSize - 1))) { delete vhd; return INVALID_DATA; }

	//if differencing, try to open parent
	if (footer.diskType == VHD_TYPE_DIFFERENCING) {

		//todo: remove these lines of code to enable diffencing drives (must implement TryOpenParent to be effective)
		LOG_MSG("Differential VHD drives not implemented.\n");
		delete vhd;
		return UNSUPPORTED_TYPE;

		//loop through each reference and try to find one we can open
		for (int i = 0; i < 8; i++) {
			//ignore entries with platform code 'none'
			if (dynHeader.parentLocatorEntry[i].platformCode != 0) {
				//load the platform data, if there is any
				Bit64u dataOffset = dynHeader.parentLocatorEntry[i].platformDataOffset;
				Bit32u dataLength = dynHeader.parentLocatorEntry[i].platformDataLength;
				Bit8u* buffer = 0;
				if (dataOffset && dataLength && ((Bit64u)dataOffset + dataLength) <= footerPosition) {
					if (fseeko64(file, dataOffset, SEEK_SET)) { delete vhd; return INVALID_DATA; }
					buffer = (Bit8u*)malloc(dataLength + 2);
					if (buffer == 0) { delete vhd; return INVALID_DATA; }
					if (fread(buffer, sizeof(Bit8u), dataLength, file) != dataLength) { free(buffer); delete vhd; return INVALID_DATA; }
					buffer[dataLength] = 0; //append null character, just in case
					buffer[dataLength + 1] = 0; //two bytes, because this might be UTF-16
				}
				ErrorCodes ret = TryOpenParent(fileName, dynHeader.parentLocatorEntry[i], buffer, buffer ? dataLength : 0, &(vhd->parentDisk), dynHeader.parentUniqueId);
				if (buffer) free(buffer);
				if (vhd->parentDisk) break; //success
				if (ret != ERROR_OPENING) {
					delete vhd;
					return (ErrorCodes)(ret | PARENT_ERROR);
				}
			}
		}
		//check and see if we were successful in opening a file
		if (vhd->parentDisk == 0) { delete vhd; return ERROR_OPENING_PARENT; }
	}

	//calculate sectors per block
	Bit32u sectorsPerBlock = dynHeader.blockSize / 512;
	//calculate block map size
	Bit32u blockMapSectors = ((
		((sectorsPerBlock /* 4096 sectors/block (typ) = 4096 bits required */
		+ 7) / 8) /* convert to bytes and round up to nearest byte; 4096/8 = 512 bytes required */
		+ 511) / 512); /* convert to sectors and round up to nearest sector; 512/512 = 1 sector */
	//check that the BAT is large enough for the disk
	Bit32u tablesRequired = (Bit32u)((calcDiskSize + (dynHeader.blockSize - 1)) / dynHeader.blockSize);
	if (dynHeader.maxTableEntries < tablesRequired) { delete vhd; return INVALID_DATA; }
	//check that the BAT is contained within the file
	if (((Bit64u)dynHeader.tableOffset + ((Bit64u)dynHeader.maxTableEntries * (Bit64u)4)) > footerPosition) { delete vhd; return INVALID_DATA; }

	//set remaining variables
	vhd->dynamicHeader = dynHeader;
	vhd->blockMapSectors = blockMapSectors;
	vhd->blockMapSize = blockMapSectors * 512;
	vhd->sectorsPerBlock = sectorsPerBlock;
	vhd->currentBlockDirtyMap = (Bit8u*)malloc(vhd->blockMapSize);
	if (vhd->currentBlockDirtyMap == 0) { delete vhd; return INVALID_DATA; }

	//try loading the first block
	if (!vhd->loadBlock(0)) { 
		delete vhd; 
		return INVALID_DATA; 
	}

	*disk = vhd;
	return OPEN_SUCCESS;
}

imageDiskVHD::ErrorCodes imageDiskVHD::TryOpenParent(const char* childFileName, const imageDiskVHD::ParentLocatorEntry &entry, Bit8u* data, const Bit32u dataLength, imageDisk** disk, const char* uniqueId) {
	switch (entry.platformCode) {
	case 0x57327275:
		//Unicode relative pathname (UTF-16) on Windows

		//todo: convert string to ASCII and append null  (or modify code to use wide chars)
		//todo: consider existing file path, and create relative path
		//return imageDiskVHD::Open(newFileName, true, disk, uniqueId);
		break;
	case 0x57326B75:
		//Unicode absolute pathname (UTF-16) on Windows
		
		//todo: convert string to ASCII and append null
		//return imageDiskVHD::Open(newFileName, true, disk, uniqueId);
		break;
	case 0x4D616320:
		//Mac OS alias stored as blob
		break;
	case 0x4D616358:
		//Mac OSX file url (RFC 2396)
		break;
	}
	return ERROR_OPENING; //return ERROR_OPENING if the file does not exist, cannot be accessed, etc
}

Bit8u imageDiskVHD::Read_AbsoluteSector(Bit32u sectnum, void * data) {
	Bit32u blockNumber = sectnum / sectorsPerBlock;
	Bit32u sectorOffset = sectnum % sectorsPerBlock;
	if (!loadBlock(blockNumber)) return 0x05; //can't load block
	if (currentBlockAllocated) {
		Bit32u byteNum = sectorOffset / 8;
		Bit32u bitNum = sectorOffset % 8;
		bool hasData = currentBlockDirtyMap[byteNum] & (1 << (7 - bitNum));
		if (hasData) {
			if (fseeko64(diskimg, ((Bit64u)currentBlockSectorOffset + blockMapSectors + sectorOffset) * 512, SEEK_SET)) return 0x05; //can't seek
			if (fread(data, sizeof(Bit8u), 512, diskimg) != 512) return 0x05; //can't read
			return 0;
		}
	}
	if (parentDisk) {
		return parentDisk->Read_AbsoluteSector(sectnum, data);
	}
	else {
		memset(data, 0, 512);
		return 0;
	}
}

Bit8u imageDiskVHD::Write_AbsoluteSector(Bit32u sectnum, void * data) {
	Bit32u blockNumber = sectnum / sectorsPerBlock;
	Bit32u sectorOffset = sectnum % sectorsPerBlock;
	if (!loadBlock(blockNumber)) return 0x05; //can't load block
	if (!currentBlockAllocated) {
		if (!copiedFooter) {
			//write backup of footer at start of file (should already exist, but we never checked to be sure it is readable or matches the footer we used)
			if (fseeko64(diskimg, 0, SEEK_SET)) return 0x05;
			if (fwrite(originalFooter.cookie, sizeof(Bit8u), 512, diskimg) != 512) return 0x05;
			copiedFooter = true;
			//flush the data to disk after writing the backup footer
			if (fflush(diskimg)) return 0x05;
		}
		//calculate new location of footer, and round up to nearest 512 byte increment "just in case"
		Bit64u newFooterPosition = (((footerPosition - 512 + blockMapSize + dynamicHeader.blockSize) + 511) / 512) * 512;
		//attempt to extend the length appropriately first (on some operating systems this will extend the file)
		if (fseeko64(diskimg, newFooterPosition + 512, SEEK_SET)) return 0x05;
		//now write the footer
		if (fseeko64(diskimg, newFooterPosition, SEEK_SET)) return 0x05;
		if (fwrite(originalFooter.cookie, sizeof(Bit8u), 512, diskimg) != 512) return 0x05;
		//save the new block location and new footer position
		Bit32u newBlockSectorNumber = (Bit32u)((footerPosition + 511) / 512);
		footerPosition = newFooterPosition;
		//clear the dirty flags for the new footer position
		for (Bit32u i = 0; i < blockMapSize; i++) currentBlockDirtyMap[i] = 0;
		//write the dirty map
		if (fseeko64(diskimg, newBlockSectorNumber * 512, SEEK_SET)) return 0x05;
		if (fwrite(currentBlockDirtyMap, sizeof(Bit8u), blockMapSize, diskimg) != blockMapSize) return 0x05;
		//flush the data to disk after expanding the file, before allocating the block in the BAT
		if (fflush(diskimg)) return 0x05;
		//update the BAT
		if (fseeko64(diskimg, dynamicHeader.tableOffset + (blockNumber * 4), SEEK_SET)) return 0x05;
		Bit32u newBlockSectorNumberBE = SDL_SwapBE32(newBlockSectorNumber);
		if (fwrite(&newBlockSectorNumberBE, sizeof(Bit8u), 4, diskimg) != 4) return false;
		currentBlockAllocated = true;
		currentBlockSectorOffset = newBlockSectorNumber;
		//flush the data to disk after allocating a block
		if (fflush(diskimg)) return 0x05;
	}
	//current block has now been allocated
	Bit32u byteNum = sectorOffset / 8;
	Bit32u bitNum = sectorOffset % 8;
	bool hasData = currentBlockDirtyMap[byteNum] & (1 << (7 - bitNum));
	//if the sector hasn't been marked as dirty, mark it as dirty
	if (!hasData) {
		currentBlockDirtyMap[byteNum] |= 1 << (7 - bitNum);
		if (fseeko64(diskimg, currentBlockSectorOffset * 512, SEEK_SET)) return 0x05; //can't seek
		if (fwrite(currentBlockDirtyMap, sizeof(Bit8u), blockMapSize, diskimg) != blockMapSize) return 0x05;
	}
	//current sector has now been marked as dirty
	//write the sector
	if (fseeko64(diskimg, ((Bit64u)currentBlockSectorOffset + blockMapSectors + sectorOffset) * 512, SEEK_SET)) return 0x05; //can't seek
	if (fwrite(data, sizeof(Bit8u), 512, diskimg) != 512) return 0x05; //can't write
	return 0;
}

imageDiskVHD::VHDTypes imageDiskVHD::GetVHDType(const char* fileName) {
	imageDisk* disk;
	if (Open(fileName, true, &disk)) return VHD_TYPE_NONE;
	imageDiskVHD* vhd = dynamic_cast<imageDiskVHD*>(disk);
	VHDTypes ret = VHD_TYPE_FIXED; //fixed if an imageDisk was returned
	if (vhd) ret = vhd->footer.diskType; //get the actual type if an imageDiskVHD was returned
	delete disk;
	return ret;
}

bool imageDiskVHD::loadBlock(const Bit32u blockNumber) {
	if (currentBlock == blockNumber) return true;
	if (fseeko64(diskimg, dynamicHeader.tableOffset + (blockNumber * 4), SEEK_SET)) return false;
	Bit32u blockSectorOffset;
	if (fread(&blockSectorOffset, sizeof(Bit8u), 4, diskimg) != 4) return false;
	blockSectorOffset = SDL_SwapBE32(blockSectorOffset);
	if (blockSectorOffset == 0xFFFFFFFF) {
		currentBlock = blockNumber;
		currentBlockAllocated = false;
	}
	else {
		if (fseeko64(diskimg, blockSectorOffset * (Bit64u)512, SEEK_SET)) return false;
		currentBlock = 0xFFFFFFFF;
		currentBlockAllocated = true;
		currentBlockSectorOffset = blockSectorOffset;
		if (fread(currentBlockDirtyMap, sizeof(Bit8u), blockMapSize, diskimg) != blockMapSize) return false;
		currentBlock = blockNumber;
	}
	return true;
}

imageDiskVHD::~imageDiskVHD() {
	if (currentBlockDirtyMap) {
		free(currentBlockDirtyMap);
		currentBlockDirtyMap = 0;
	}
	if (parentDisk) {
		parentDisk->Release();
		parentDisk = 0;
	}
}

void imageDiskVHD::VHDFooter::SwapByteOrder() {
	features = SDL_SwapBE32(features);
	fileFormatVersion = SDL_SwapBE32(fileFormatVersion);
	dataOffset = SDL_SwapBE64(dataOffset);
	timeStamp = SDL_SwapBE32(timeStamp);
	creatorVersion = SDL_SwapBE32(creatorVersion);
	creatorHostOS = SDL_SwapBE32(creatorHostOS);
	originalSize = SDL_SwapBE64(originalSize);
	currentSize = SDL_SwapBE64(currentSize);
	geometry.cylinders = SDL_SwapBE16(geometry.cylinders);
	diskType = (VHDTypes)SDL_SwapBE32((Bit32u)diskType);
	checksum = SDL_SwapBE32(checksum);
	//guid might need the byte order swapped also
	//however, for our purposes (comparing to the parent guid on differential disks),
	//  it doesn't matter so long as we are consistent
}

Bit32u imageDiskVHD::VHDFooter::CalculateChecksum() {
	//checksum is one's complement of sum of bytes excluding the checksum
	//because of that, the byte order doesn't matter when calculating the checksum
	//however, the checksum must be stored in the correct byte order or it will not match

	Bit32u ret = 0;
	Bit8u* dat = (Bit8u*)this->cookie;
	Bit32u oldChecksum = checksum;
	checksum = 0;
	for (int i = 0; i < sizeof(VHDFooter); i++) {
		ret += dat[i];
	}
	checksum = oldChecksum;
	return ~ret;
}

bool imageDiskVHD::VHDFooter::IsValid() {
	return (
		memcmp(cookie, "conectix", 8) == 0 &&
		fileFormatVersion >= 0x00010000 &&
		fileFormatVersion <= 0x0001FFFF &&
		checksum == CalculateChecksum());
}

void imageDiskVHD::DynamicHeader::SwapByteOrder() {
	dataOffset = SDL_SwapBE64(dataOffset);
	tableOffset = SDL_SwapBE64(tableOffset);
	headerVersion = SDL_SwapBE32(headerVersion);
	maxTableEntries = SDL_SwapBE32(maxTableEntries);
	blockSize = SDL_SwapBE32(blockSize);
	checksum = SDL_SwapBE32(checksum);
	parentTimeStamp = SDL_SwapBE32(parentTimeStamp);
	for (int i = 0; i < 256; i++) {
		parentUnicodeName[i] = SDL_SwapBE16(parentUnicodeName[i]);
	}
	for (int i = 0; i < 8; i++) {
		parentLocatorEntry[i].platformCode = SDL_SwapBE32(parentLocatorEntry[i].platformCode);
		parentLocatorEntry[i].platformDataSpace = SDL_SwapBE32(parentLocatorEntry[i].platformDataSpace);
		parentLocatorEntry[i].platformDataLength = SDL_SwapBE32(parentLocatorEntry[i].platformDataLength);
		parentLocatorEntry[i].reserved = SDL_SwapBE32(parentLocatorEntry[i].reserved);
		parentLocatorEntry[i].platformDataOffset = SDL_SwapBE64(parentLocatorEntry[i].platformDataOffset);
	}
	//parent guid might need the byte order swapped also
	//however, for our purposes (comparing to the parent guid on differential disks),
	//  it doesn't matter so long as we are consistent
}

Bit32u imageDiskVHD::DynamicHeader::CalculateChecksum() {
	//checksum is one's complement of sum of bytes excluding the checksum
	//because of that, the byte order doesn't matter when calculating the checksum
	//however, the checksum must be stored in the correct byte order or it will not match

	Bit32u ret = 0;
	Bit8u* dat = (Bit8u*)this->cookie;
	Bit32u oldChecksum = checksum;
	checksum = 0;
	for (int i = 0; i < sizeof(DynamicHeader); i++) {
		ret += dat[i];
	}
	checksum = oldChecksum;
	return ~ret;
}

bool imageDiskVHD::DynamicHeader::IsValid() {
	return (
		memcmp(cookie, "cxsparse", 8) == 0 &&
		headerVersion >= 0x00010000 &&
		headerVersion <= 0x0001FFFF &&
		checksum == CalculateChecksum());
}
