package com.retiredroca.remoteworker.platform;

/** Loader-specific services used by the shared (common) code. */
public interface ExampleModPlatform {
    String loaderName();

    boolean isDevelopmentEnvironment();
}
