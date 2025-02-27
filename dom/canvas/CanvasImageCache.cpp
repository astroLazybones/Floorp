/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CanvasImageCache.h"
#include "nsIImageLoadingContent.h"
#include "nsExpirationTracker.h"
#include "imgIRequest.h"
#include "mozilla/dom/Element.h"
#include "nsTHashtable.h"
#include "nsContentUtils.h"
#include "mozilla/Preferences.h"
#include "mozilla/StaticPrefs_canvas.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/gfx/2D.h"
#include "gfx2DGlue.h"

namespace mozilla {

using namespace dom;
using namespace gfx;

/**
 * Used for images specific to this one canvas. Required
 * due to CORS security.
 */
struct ImageCacheKey {
  ImageCacheKey(imgIContainer* aImage, CanvasRenderingContext2D* aContext,
                BackendType aBackendType)
      : mImage(aImage), mContext(aContext), mBackendType(aBackendType) {}
  nsCOMPtr<imgIContainer> mImage;
  CanvasRenderingContext2D* mContext;
  BackendType mBackendType;
};

/**
 * Cache data needs to be separate from the entry
 * for nsExpirationTracker.
 */
struct ImageCacheEntryData {
  ImageCacheEntryData(const ImageCacheEntryData& aOther)
      : mImage(aOther.mImage),
        mContext(aOther.mContext),
        mBackendType(aOther.mBackendType),
        mSourceSurface(aOther.mSourceSurface),
        mSize(aOther.mSize),
        mIntrinsicSize(aOther.mIntrinsicSize) {}
  explicit ImageCacheEntryData(const ImageCacheKey& aKey)
      : mImage(aKey.mImage),
        mContext(aKey.mContext),
        mBackendType(aKey.mBackendType) {}

  nsExpirationState* GetExpirationState() { return &mState; }
  size_t SizeInBytes() { return mSize.width * mSize.height * 4; }

  // Key
  nsCOMPtr<imgIContainer> mImage;
  CanvasRenderingContext2D* mContext;
  BackendType mBackendType;
  // Value
  RefPtr<SourceSurface> mSourceSurface;
  IntSize mSize;
  IntSize mIntrinsicSize;
  nsExpirationState mState;
};

class ImageCacheEntry : public PLDHashEntryHdr {
 public:
  using KeyType = ImageCacheKey;
  using KeyTypePointer = const ImageCacheKey*;

  explicit ImageCacheEntry(const KeyType* aKey)
      : mData(new ImageCacheEntryData(*aKey)) {}
  ImageCacheEntry(const ImageCacheEntry& toCopy)
      : mData(new ImageCacheEntryData(*toCopy.mData)) {}
  ~ImageCacheEntry() = default;

  bool KeyEquals(KeyTypePointer key) const {
    return mData->mImage == key->mImage && mData->mContext == key->mContext &&
           mData->mBackendType == key->mBackendType;
  }

  static KeyTypePointer KeyToPointer(KeyType& key) { return &key; }
  static PLDHashNumber HashKey(KeyTypePointer key) {
    return HashGeneric(key->mImage.get(), key->mContext, key->mBackendType);
  }
  enum { ALLOW_MEMMOVE = true };

  UniquePtr<ImageCacheEntryData> mData;
};

/**
 * Used for all images across all canvases.
 */
struct AllCanvasImageCacheKey {
  explicit AllCanvasImageCacheKey(imgIContainer* aImage,
                                  BackendType aBackendType)
      : mImage(aImage), mBackendType(aBackendType) {}

  nsCOMPtr<imgIContainer> mImage;
  BackendType mBackendType;
};

class AllCanvasImageCacheEntry : public PLDHashEntryHdr {
 public:
  using KeyType = AllCanvasImageCacheKey;
  using KeyTypePointer = const AllCanvasImageCacheKey*;

  explicit AllCanvasImageCacheEntry(const KeyType* aKey)
      : mImage(aKey->mImage), mBackendType(aKey->mBackendType) {}

  AllCanvasImageCacheEntry(const AllCanvasImageCacheEntry& toCopy)
      : mImage(toCopy.mImage),
        mSourceSurface(toCopy.mSourceSurface),
        mBackendType(toCopy.mBackendType) {}

  ~AllCanvasImageCacheEntry() = default;

  bool KeyEquals(KeyTypePointer key) const {
    return mImage == key->mImage && mBackendType == key->mBackendType;
  }

  static KeyTypePointer KeyToPointer(KeyType& key) { return &key; }
  static PLDHashNumber HashKey(KeyTypePointer key) {
    return HashGeneric(key->mImage.get(), key->mBackendType);
  }
  enum { ALLOW_MEMMOVE = true };

  nsCOMPtr<imgIContainer> mImage;
  RefPtr<SourceSurface> mSourceSurface;
  BackendType mBackendType;
};

class ImageCacheObserver;

class ImageCache final : public nsExpirationTracker<ImageCacheEntryData, 4> {
 public:
  // We use 3 generations of 1 second each to get a 2-3 seconds timeout.
  enum { GENERATION_MS = 1000 };
  ImageCache();
  ~ImageCache();

  virtual void NotifyExpired(ImageCacheEntryData* aObject) override {
    mTotal -= aObject->SizeInBytes();
    RemoveObject(aObject);

    // Remove from the all canvas cache entry first since nsExpirationTracker
    // will delete aObject.
    mAllCanvasCache.RemoveEntry(
        AllCanvasImageCacheKey(aObject->mImage, aObject->mBackendType));

    // Deleting the entry will delete aObject since the entry owns aObject.
    mCache.RemoveEntry(ImageCacheKey(aObject->mImage, aObject->mContext,
                                     aObject->mBackendType));
  }

  nsTHashtable<ImageCacheEntry> mCache;
  nsTHashtable<AllCanvasImageCacheEntry> mAllCanvasCache;
  size_t mTotal;
  RefPtr<ImageCacheObserver> mImageCacheObserver;
};

static ImageCache* gImageCache = nullptr;

// Listen memory-pressure event for image cache purge.
class ImageCacheObserver final : public nsIObserver {
 public:
  NS_DECL_ISUPPORTS

  explicit ImageCacheObserver(ImageCache* aImageCache)
      : mImageCache(aImageCache) {
    RegisterObserverEvents();
  }

  void Destroy() {
    UnregisterObserverEvents();
    mImageCache = nullptr;
  }

  NS_IMETHOD Observe(nsISupports* aSubject, const char* aTopic,
                     const char16_t* aSomeData) override {
    if (!mImageCache || (strcmp(aTopic, "memory-pressure") != 0 &&
                         strcmp(aTopic, "canvas-device-reset") != 0)) {
      return NS_OK;
    }

    mImageCache->AgeAllGenerations();
    return NS_OK;
  }

 private:
  virtual ~ImageCacheObserver() = default;

  void RegisterObserverEvents() {
    nsCOMPtr<nsIObserverService> observerService =
        mozilla::services::GetObserverService();

    MOZ_ASSERT(observerService);

    if (observerService) {
      observerService->AddObserver(this, "memory-pressure", false);
      observerService->AddObserver(this, "canvas-device-reset", false);
    }
  }

  void UnregisterObserverEvents() {
    nsCOMPtr<nsIObserverService> observerService =
        mozilla::services::GetObserverService();

    // Do not assert on observerService here. This might be triggered by
    // the cycle collector at a late enough time, that XPCOM services are
    // no longer available. See bug 1029504.
    if (observerService) {
      observerService->RemoveObserver(this, "memory-pressure");
      observerService->RemoveObserver(this, "canvas-device-reset");
    }
  }

  ImageCache* mImageCache;
};

NS_IMPL_ISUPPORTS(ImageCacheObserver, nsIObserver)

class CanvasImageCacheShutdownObserver final : public nsIObserver {
  ~CanvasImageCacheShutdownObserver() = default;

 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIOBSERVER
};

ImageCache::ImageCache()
    : nsExpirationTracker<ImageCacheEntryData, 4>(GENERATION_MS, "ImageCache"),
      mTotal(0) {
  mImageCacheObserver = new ImageCacheObserver(this);
  MOZ_RELEASE_ASSERT(mImageCacheObserver,
                     "GFX: Can't alloc ImageCacheObserver");
}

ImageCache::~ImageCache() {
  AgeAllGenerations();
  mImageCacheObserver->Destroy();
}

static already_AddRefed<imgIContainer> GetImageContainer(dom::Element* aImage) {
  nsCOMPtr<imgIRequest> request;
  nsCOMPtr<nsIImageLoadingContent> ilc = do_QueryInterface(aImage);
  if (!ilc) {
    return nullptr;
  }

  ilc->GetRequest(nsIImageLoadingContent::CURRENT_REQUEST,
                  getter_AddRefs(request));
  if (!request) {
    return nullptr;
  }

  nsCOMPtr<imgIContainer> imgContainer;
  request->GetImage(getter_AddRefs(imgContainer));
  if (!imgContainer) {
    return nullptr;
  }

  return imgContainer.forget();
}

void CanvasImageCache::NotifyCanvasDestroyed(
    CanvasRenderingContext2D* aContext) {
  MOZ_ASSERT(aContext);

  if (!NS_IsMainThread() || !gImageCache) {
    return;
  }

  for (auto i = gImageCache->mCache.Iter(); !i.Done(); i.Next()) {
    ImageCacheEntryData* data = i.Get()->mData.get();
    if (data->mContext == aContext) {
      gImageCache->RemoveObject(data);
      gImageCache->mAllCanvasCache.RemoveEntry(
          AllCanvasImageCacheKey(data->mImage, data->mBackendType));
      i.Remove();
    }
  }
}

void CanvasImageCache::NotifyDrawImage(Element* aImage,
                                       CanvasRenderingContext2D* aContext,
                                       DrawTarget* aTarget,
                                       SourceSurface* aSource,
                                       const IntSize& aSize,
                                       const IntSize& aIntrinsicSize) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(aContext);

  if (!aTarget || !aContext) {
    return;
  }

  if (!gImageCache) {
    gImageCache = new ImageCache();
    nsContentUtils::RegisterShutdownObserver(
        new CanvasImageCacheShutdownObserver());
  }

  nsCOMPtr<imgIContainer> imgContainer = GetImageContainer(aImage);
  if (!imgContainer) {
    return;
  }

  BackendType backendType = aTarget->GetBackendType();
  AllCanvasImageCacheKey allCanvasCacheKey(imgContainer, backendType);
  ImageCacheKey canvasCacheKey(imgContainer, aContext, backendType);
  ImageCacheEntry* entry = gImageCache->mCache.PutEntry(canvasCacheKey);

  if (entry) {
    if (entry->mData->mSourceSurface) {
      // We are overwriting an existing entry.
      gImageCache->mTotal -= entry->mData->SizeInBytes();
      gImageCache->RemoveObject(entry->mData.get());
      gImageCache->mAllCanvasCache.RemoveEntry(allCanvasCacheKey);
    }

    gImageCache->AddObject(entry->mData.get());
    entry->mData->mSourceSurface = aSource;
    entry->mData->mSize = aSize;
    entry->mData->mIntrinsicSize = aIntrinsicSize;
    gImageCache->mTotal += entry->mData->SizeInBytes();

    AllCanvasImageCacheEntry* allEntry =
        gImageCache->mAllCanvasCache.PutEntry(allCanvasCacheKey);
    if (allEntry) {
      allEntry->mSourceSurface = aSource;
    }
  }

  if (!StaticPrefs::canvas_image_cache_limit()) {
    return;
  }

  // Expire the image cache early if its larger than we want it to be.
  while (gImageCache->mTotal >
         size_t(StaticPrefs::canvas_image_cache_limit())) {
    gImageCache->AgeOneGeneration();
  }
}

SourceSurface* CanvasImageCache::LookupAllCanvas(Element* aImage,
                                                 DrawTarget* aTarget) {
  MOZ_ASSERT(NS_IsMainThread());

  if (!gImageCache || !aTarget) {
    return nullptr;
  }

  nsCOMPtr<imgIContainer> imgContainer = GetImageContainer(aImage);
  if (!imgContainer) {
    return nullptr;
  }

  AllCanvasImageCacheEntry* entry = gImageCache->mAllCanvasCache.GetEntry(
      AllCanvasImageCacheKey(imgContainer, aTarget->GetBackendType()));
  if (!entry) {
    return nullptr;
  }

  return entry->mSourceSurface;
}

SourceSurface* CanvasImageCache::LookupCanvas(
    Element* aImage, CanvasRenderingContext2D* aContext, DrawTarget* aTarget,
    IntSize* aSizeOut, IntSize* aIntrinsicSizeOut) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(aContext);

  if (!gImageCache || !aTarget) {
    return nullptr;
  }

  nsCOMPtr<imgIContainer> imgContainer = GetImageContainer(aImage);
  if (!imgContainer) {
    return nullptr;
  }

  // We filter based on the backend type because we don't want a surface that
  // was optimized for one backend to be preferred for another backend, as this
  // could have performance implications. For example, a Skia optimized surface
  // given to a D2D backend would cause an upload each time, and similarly a D2D
  // optimized surface given to a Skia backend would cause a readback. For
  // details, see bug 1794442.
  ImageCacheEntry* entry = gImageCache->mCache.GetEntry(
      ImageCacheKey(imgContainer, aContext, aTarget->GetBackendType()));
  if (!entry) {
    return nullptr;
  }

  MOZ_ASSERT(aSizeOut);

  gImageCache->MarkUsed(entry->mData.get());
  *aSizeOut = entry->mData->mSize;
  *aIntrinsicSizeOut = entry->mData->mIntrinsicSize;
  return entry->mData->mSourceSurface;
}

NS_IMPL_ISUPPORTS(CanvasImageCacheShutdownObserver, nsIObserver)

NS_IMETHODIMP
CanvasImageCacheShutdownObserver::Observe(nsISupports* aSubject,
                                          const char* aTopic,
                                          const char16_t* aData) {
  if (strcmp(aTopic, NS_XPCOM_SHUTDOWN_OBSERVER_ID) == 0) {
    delete gImageCache;
    gImageCache = nullptr;

    nsContentUtils::UnregisterShutdownObserver(this);
  }

  return NS_OK;
}

}  // namespace mozilla
