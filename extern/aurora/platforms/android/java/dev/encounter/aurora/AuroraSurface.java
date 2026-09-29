package dev.encounter.aurora;

import android.content.Context;
import android.view.SurfaceHolder;

import org.libsdl.app.SDLSurface;

public class AuroraSurface extends SDLSurface {
    private static native void nativeSetSurfaceReady(boolean ready);
    private static native void nativeSetSurfaceChanging(boolean changing);
    private static native boolean nativeWaitSurfaceReleased();

    public AuroraSurface(Context context) {
        super(context);
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        nativeSetSurfaceReady(false);
        super.surfaceCreated(holder);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        nativeSetSurfaceReady(false);
        // The window must be unused once this returns; let the renderer drop its
        // swapchain first (bounded, so a busy main thread can't cause an ANR).
        nativeWaitSurfaceReleased();
        super.surfaceDestroyed(holder);
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        // The native window survives surfaceChanged, so keep the renderer's surface:
        // a second one for the same window fails while the old swapchain is connected.
        nativeSetSurfaceChanging(true);
        super.surfaceChanged(holder, format, width, height);
        nativeSetSurfaceReady(mIsSurfaceReady);
        nativeSetSurfaceChanging(false);
    }
}
