/*
 * Copyright (c) 2017 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

#include <string.h>

#include <string>
#include <map>
#include <vector>

#include "ClosurePrinter.h"
#include "JSONWriter.h"

using namespace dyld3::json;

namespace dyld3 {
namespace closure {

static std::string printTarget(const Array<const ImageArray*>& imagesArrays, Image::ResolvedSymbolTarget target)
{
	const Image* targetImage;
	uint64_t value;
    switch ( target.image.kind ) {
        case Image::ResolvedSymbolTarget::kindImage:
            targetImage = ImageArray::findImage(imagesArrays, target.image.imageNum);
            if ( target.image.offset & 0x8000000000ULL ) {
                uint64_t signExtend = target.image.offset | 0xFFFFFF0000000000ULL;
                return std::string("bind to ") + targetImage->leafName() + " - " + hex8(-signExtend);
            }
            else
                return std::string("bind to ") + targetImage->leafName() + " + " + hex8(target.image.offset);
            break;
        case Image::ResolvedSymbolTarget::kindSharedCache:
            return std::string("bind to dyld cache + ") + hex8(target.sharedCache.offset);
            break;
        case Image::ResolvedSymbolTarget::kindAbsolute:
            value = target.absolute.value;
            if ( value & 0x2000000000000000LL )
                value |= 0xC000000000000000LL;
            return std::string("bind to absolute ") + hex(value);
            break;
    }
    return "???";
}


static Node buildImageNode(const Image* image, const Array<const ImageArray*>& imagesArrays, bool printFixups, bool printDependentsDetails, const uint8_t* cacheStart=nullptr)
{
    __block Node imageNode;

    if ( image->isInvalid() )
        return imageNode;

    imageNode.map["image-num"].value = hex4(image->imageNum());
    imageNode.map["path"].value = image->path();
    __block Node imageAliases;
    image->forEachAlias(^(const char* aliasPath, bool& stop) {
        Node anAlias;
        anAlias.value = aliasPath;
        imageAliases.array.push_back(anAlias);
    });
    if ( !imageAliases.array.empty() )
        imageNode.map["aliases"] = imageAliases;
    uuid_t uuid;
    if ( image->getUuid(uuid) ) {
        uuid_string_t uuidStr;
        uuid_unparse(uuid, uuidStr);
        imageNode.map["uuid"].value = uuidStr;
    }
    imageNode.map["has-objc"].value = (image->hasObjC() ? "true" : "false");
    imageNode.map["has-weak-defs"].value = (image->hasWeakDefs() ? "true" : "false");
    imageNode.map["has-plus-loads"].value = (image->mayHavePlusLoads() ? "true" : "false");
    imageNode.map["never-unload"].value = (image->neverUnload() ? "true" : "false");
//    imageNode.map["platform-binary"].value = (image->isPlatformBinary() ? "true" : "false");
//    if ( image->cwdMustBeThisDir() )
//        imageNode.map["cwd-must-be-this-dir"].value = "true";
    if ( !image->inDyldCache() ) {
        uint32_t csFileOffset;
        uint32_t csSize;
        if ( image->hasCodeSignature(csFileOffset, csSize) ) {
            imageNode.map["code-sign-location"].map["offset"].value = hex(csFileOffset);
            imageNode.map["code-sign-location"].map["size"].value = hex(csSize);
        }
//        uint32_t fpTextOffset;
//        uint32_t fpSize;
//        if ( image->isFairPlayEncrypted(fpTextOffset, fpSize) ) {
//            imageNode.map["fairplay-encryption-location"].map["offset"].value = hex(fpTextOffset);
//            imageNode.map["fairplay-encryption-location"].map["size"].value = hex(fpSize);
//        }
        uint64_t inode;
        uint64_t mTime;
        if ( image->hasFileModTimeAndInode(inode, mTime) ) {
            imageNode.map["file-mod-time"].value = hex(inode);
            imageNode.map["file-inode"].value = hex(mTime);
        }
        uint8_t cdHash[20];
        if ( image->hasCdHash(cdHash) ) {
            std::string cdHashStr;
            cdHashStr.reserve(24);
            for (int i=0; i < 20; ++i) {
                uint8_t byte = cdHash[i];
                uint8_t nibbleL = byte & 0x0F;
                uint8_t nibbleH = byte >> 4;
                if ( nibbleH < 10 )
                    cdHashStr += '0' + nibbleH;
                else
                    cdHashStr += 'a' + (nibbleH-10);
                if ( nibbleL < 10 )
                    cdHashStr += '0' + nibbleL;
                else
                    cdHashStr += 'a' + (nibbleL-10);
            }
            if ( cdHashStr != "0000000000000000000000000000000000000000" )
                imageNode.map["cd-hash"].value = cdHashStr;
        }
        else {
    #if 0
            const uint8_t* cdHash = image->cdHash16();
            std::string cdHashStr;
            cdHashStr.reserve(32);
            for (int j=0; j < 16; ++j) {
                uint8_t byte = cdHash[j];
                uint8_t nibbleL = byte & 0x0F;
                uint8_t nibbleH = byte >> 4;
                if ( nibbleH < 10 )
                    cdHashStr += '0' + nibbleH;
                else
                    cdHashStr += 'a' + (nibbleH-10);
                if ( nibbleL < 10 )
                    cdHashStr += '0' + nibbleL;
                else
                    cdHashStr += 'a' + (nibbleL-10);
            }
            imageNode.map["file-cd-hash-16"].value = cdHashStr;
    #endif
        }
        imageNode.map["total-vm-size"].value = hex(image->vmSizeToMap());
        uint64_t sliceOffset = image->sliceOffsetInFile();
        if ( sliceOffset != 0 )
            imageNode.map["file-offset-of-slice"].value = hex(sliceOffset);
        //if ( image->hasTextRelocs() )
        //    imageNode.map["has-text-relocs"].value = "true";
        image->forEachDiskSegment(^(uint32_t segIndex, uint32_t fileOffset, uint32_t fileSize, int64_t vmOffset, uint64_t vmSize, uint8_t permissions, bool& stop) {
            Node segInfoNode;
            segInfoNode.map["file-offset"].value = hex(fileOffset);
            segInfoNode.map["file-size"].value = hex(fileSize);
            segInfoNode.map["vm-size"].value = hex(vmSize);
            segInfoNode.map["permissions"].value = hex(permissions);
            imageNode.map["mappings"].array.push_back(segInfoNode);
        });



        if ( printFixups ) {
            image->forEachFixup(^(uint64_t imageOffsetToRebase, bool &stop) {
                // rebase
                imageNode.map["fixups"].map[hex8(imageOffsetToRebase)].value = "rebase";
            }, ^(uint64_t imageOffsetToBind, Image::ResolvedSymbolTarget target, bool &stop) {
                // bind
                imageNode.map["fixups"].map[hex8(imageOffsetToBind)].value = printTarget(imagesArrays, target);
            }, ^(uint64_t imageOffsetStart, const Array<Image::ResolvedSymbolTarget>& targets, bool& stop) {
                // chain
                imageNode.map["fixups"].map[hex8(imageOffsetStart)].value = "chain-start";
                for (const Image::ResolvedSymbolTarget& target: targets) {
                    Node targetNode;
                    targetNode.value = printTarget(imagesArrays, target);
                    imageNode.map["fixups-targets"].array.push_back(targetNode);
                }
            });
            image->forEachTextReloc(^(uint32_t imageOffsetToRebase, bool &stop) {
                // rebase
                imageNode.map["fixups"].map[hex8(imageOffsetToRebase)].value = "text rebase";
            }, ^(uint32_t imageOffsetToBind, Image::ResolvedSymbolTarget target, bool &stop) {
                imageNode.map["fixups"].map[hex8(imageOffsetToBind)].value = "text " + printTarget(imagesArrays, target);
            });
        }
    }
    else {
        if ( printFixups ) {
            image->forEachPatchableExport(^(uint32_t cacheOffsetOfImpl, const char* name) {
                __block Node implNode;
                implNode.map["name"].value = name;
                implNode.map["impl-cache-offset"].value = hex8(cacheOffsetOfImpl);
                image->forEachPatchableUseOfExport(cacheOffsetOfImpl, ^(Image::PatchableExport::PatchLocation patchLocation) {
                    Node siteNode;
                    siteNode.map["cache-offset"].value = hex8(patchLocation.cacheOffset);
                    if ( patchLocation.addend != 0 )
                        siteNode.map["addend"].value = hex(patchLocation.addend);
                    if ( patchLocation.authenticated != 0 ) {
                        siteNode.map["key"].value = patchLocation.keyName();
                        siteNode.map["address-diversity"].value = patchLocation.usesAddressDiversity ? "true" : "false";
                        siteNode.map["discriminator"].value = hex4(patchLocation.discriminator);
                    }
                    implNode.map["usage-sites"].array.push_back(siteNode);
                });
                imageNode.map["patches"].array.push_back(implNode);
            });
        }
    }

    // add dependents
    image->forEachDependentImage(^(uint32_t depIndex, Image::LinkKind kind, ImageNum imageNum, bool& stop) {
        Node depMapNode;
        const Image* depImage = ImageArray::findImage(imagesArrays, imageNum);
        depMapNode.map["image-num"].value = hex4(imageNum);
        if ( depImage != nullptr )
            depMapNode.map["path"].value      = depImage->path();
        switch ( kind ) {
            case Image::LinkKind::regular:
                depMapNode.map["link"].value = "regular";
                break;
            case Image::LinkKind::reExport:
                depMapNode.map["link"].value = "re-export";
                break;
            case Image::LinkKind::upward:
                depMapNode.map["link"].value = "upward";
                break;
            case Image::LinkKind::weak:
                depMapNode.map["link"].value = "weak";
                break;
        }
        imageNode.map["dependents"].array.push_back(depMapNode);
    });
    
    // add initializers
    image->forEachInitializer(nullptr, ^(const void* initializer) {
        Node initNode;
        initNode.value = hex((long)initializer);
        imageNode.map["initializer-offsets"].array.push_back(initNode);
    });

	__block Node initBeforeNode;
    image->forEachImageToInitBefore(^(ImageNum imageToInit, bool& stop) {
        Node beforeNode;
        const Image* initImage = ImageArray::findImage(imagesArrays, imageToInit);
        assert(initImage != nullptr);
        beforeNode.value = initImage->path();
        imageNode.map["initializer-order"].array.push_back(beforeNode);
    });

    ImageNum cacheImageNum;
	if ( image->isOverrideOfDyldCacheImage(cacheImageNum) ) {
        imageNode.map["override-of-dyld-cache-image"].value = ImageArray::findImage(imagesArrays, cacheImageNum)->path();
	}


#if 0
    // add things to init before this image
    __block Node initBeforeNode;
    image->forEachInitBefore(groupList, ^(Image beforeImage) {
        Node beforeNode;
        beforeNode.value = beforeimage->path();
        imageNode.map["initializer-order"].array.push_back(beforeNode);
    });

     // add override info if relevant
    group.forEachImageRefOverride(groupList, ^(Image standardDylib, Image overrideDylib, bool& stop) {
        if ( overrideDylib.binaryData() == image->binaryData() ) {
            imageNode.map["override-of-cached-dylib"].value = standardDylib.path();
        }
    });
    // add dtrace info
    image->forEachDOF(nullptr, ^(const void* section) {
        Node initNode;
        initNode.value = hex((long)section);
        imageNode.map["dof-offsets"].array.push_back(initNode);
    });
#endif

    return imageNode;
}


static Node buildImageArrayNode(const ImageArray* imageArray, const Array<const ImageArray*>& imagesArrays, bool printFixups, bool printDependentsDetails, const uint8_t* cacheStart=nullptr)
{
    __block Node images;
    imageArray->forEachImage(^(const Image* image, bool& stop) {
         images.array.push_back(buildImageNode(image, imagesArrays, printFixups, printDependentsDetails, cacheStart));
    });
     return images;
}


static Node buildClosureNode(const DlopenClosure* closure, const Array<const ImageArray*>& imagesArrays, bool printFixups, bool printDependentsDetails)
{
    __block Node root;
    root.map["images"] = buildImageArrayNode(closure->images(), imagesArrays, printFixups, printDependentsDetails);

    closure->forEachPatchEntry(^(const Closure::PatchEntry& patchEntry) {
        Node patchNode;
        patchNode.map["func-dyld-cache-offset"].value = hex8(patchEntry.exportCacheOffset);
        patchNode.map["func-image-num"].value         = hex8(patchEntry.overriddenDylibInCache);
        patchNode.map["replacement"].value            = printTarget(imagesArrays, patchEntry.replacement);
        root.map["dyld-cache-fixups"].array.push_back(patchNode);
    });

    return root;
}

static Node buildClosureNode(const LaunchClosure* closure, const Array<const ImageArray*>& imagesArrays, bool printFixups, bool printDependentsDetails)
{
    __block Node root;
    root.map["images"] = buildImageArrayNode(closure->images(), imagesArrays, printFixups, printDependentsDetails);

    closure->forEachPatchEntry(^(const Closure::PatchEntry& patchEntry) {
        Node patchNode;
        patchNode.map["func-dyld-cache-offset"].value = hex8(patchEntry.exportCacheOffset);
        patchNode.map["func-image-num"].value         = hex8(patchEntry.overriddenDylibInCache);
        patchNode.map["replacement"].value            = printTarget(imagesArrays, patchEntry.replacement);
        root.map["dyld-cache-fixups"].array.push_back(patchNode);
    });

     Image::ResolvedSymbolTarget entry;
    if ( closure->mainEntry(entry) )
        root.map["main"].value = printTarget(imagesArrays, entry);
    else if ( closure->startEntry(entry) )
        root.map["start"].value = printTarget(imagesArrays, entry);

    Image::ResolvedSymbolTarget libdyldEntry;
    closure->libDyldEntry(libdyldEntry);
    root.map["libdyld-entry"].value = printTarget(imagesArrays, libdyldEntry);

    root.map["uses-@paths"].value = (closure->usedAtPaths() ? "true" : "false");
    root.map["uses-fallback-paths"].value = (closure->usedFallbackPaths() ? "true" : "false");

   // add missing files array if they exist
    closure->forEachMustBeMissingFile(^(const char* path, bool& stop) {
        Node fileNode;
        fileNode.value = path;
        root.map["must-be-missing-files"].array.push_back(fileNode);
    });

    // add interposing info, if any
    closure->forEachInterposingTuple(^(const InterposingTuple& tuple, bool& stop) {
        Node tupleNode;
        tupleNode.map["stock"].value   = printTarget(imagesArrays, tuple.stockImplementation);
        tupleNode.map["replace"].value = printTarget(imagesArrays, tuple.newImplementation);
        root.map["interposing-tuples"].array.push_back(tupleNode);
    });

    closure->forEachPatchEntry(^(const Closure::PatchEntry& patchEntry) {
        Node patchNode;
        patchNode.map["func-dyld-cache-offset"].value = hex8(patchEntry.exportCacheOffset);
        patchNode.map["func-image-num"].value         = hex8(patchEntry.overriddenDylibInCache);
        patchNode.map["replacement"].value            = printTarget(imagesArrays, patchEntry.replacement);
        root.map["dyld-cache-fixups"].array.push_back(patchNode);
    });

    root.map["initial-image-count"].value = decimal(closure->initialLoadCount());

#if 0


    // add env-vars if they exist
    closure->forEachEnvVar(^(const char* keyEqualValue, bool& stop) {
        const char* equ = strchr(keyEqualValue, '=');
        if ( equ != nullptr ) {
            char key[512];
            strncpy(key, keyEqualValue, equ-keyEqualValue);
            key[equ-keyEqualValue] = '\0';
            root.map["env-vars"].map[key].value = equ+1;
        }
    });


    // add uuid of dyld cache this closure requires
    closure.dyldCacheUUID();
    uuid_string_t cacheUuidStr;
    uuid_unparse(*closure.dyldCacheUUID(), cacheUuidStr);
    root.map["dyld-cache-uuid"].value = cacheUuidStr;

    // add top level images
    Node& rootImages = root.map["root-images"];
    uint32_t initImageCount = closure.mainExecutableImageIndex();
    rootImages.array.resize(initImageCount+1);
    for (uint32_t i=0; i <= initImageCount; ++i) {
        const Image image = closure.group().image(i);
        uuid_string_t uuidStr;
        uuid_unparse(image->uuid(), uuidStr);
        rootImages.array[i].value = uuidStr;
    }
    root.map["initial-image-count"].value = decimal(closure.initialImageCount());

    // add images
    root.map["group-num"].value = decimal(closure.group().groupNum());


    __block Node cacheOverrides;
    closure.group().forEachDyldCacheSymbolOverride(^(uint32_t patchTableIndex, uint32_t imageIndexInClosure, uint32_t imageOffset, bool& stop) {
        Node patch;
        patch.map["patch-index"].value = decimal(patchTableIndex);
        patch.map["replacement"].value = "{closure[" + decimal(imageIndexInClosure) + "]+" + hex(imageOffset) + "}";
        cacheOverrides.array.push_back(patch);
    });
    if ( !cacheOverrides.array.empty() )
        root.map["dyld-cache-overrides"].array = cacheOverrides.array;
#endif
    return root;
}

void printImageAsJSON(const Image* image, const Array<const ImageArray*>& imagesArrays, bool printFixups, FILE* out)
{
    Node root = buildImageNode(image, imagesArrays, printFixups, false);
    printJSON(root, 0, out);
}

void printDyldCacheImagesAsJSON(const DyldSharedCache* dyldCache, bool printFixups, FILE* out)
{
    const dyld3::closure::ImageArray* dylibs = dyldCache->cachedDylibsImageArray();
    STACK_ALLOC_ARRAY(const ImageArray*, imagesArrays, 2);
    imagesArrays.push_back(dylibs);

    Node root = buildImageArrayNode(dylibs, imagesArrays, printFixups, false, (uint8_t*)dyldCache);
    printJSON(root, 0, out);
}

void printClosureAsJSON(const LaunchClosure* cls, const Array<const ImageArray*>& imagesArrays, bool printFixups, FILE* out)
{
    Node root = buildClosureNode(cls, imagesArrays, printFixups, false);
    printJSON(root, 0, out);
}

void printClosureAsJSON(const DlopenClosure* cls, const Array<const ImageArray*>& imagesArrays, bool printFixups, FILE* out)
{
    Node root = buildClosureNode(cls, imagesArrays, printFixups, false);
    printJSON(root, 0, out);
}


} // namespace closure
} // namespace dyld3
