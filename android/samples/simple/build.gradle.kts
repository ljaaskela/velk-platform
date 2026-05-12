plugins {
    id("com.android.application")
}

android {
    namespace = "io.velk.samples.simple"
    compileSdk = 34
    ndkVersion = "30.0.14904198"

    defaultConfig {
        applicationId = "io.velk.samples.simple"
        minSdk = 33
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0"

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DANDROID_PLATFORM=android-33",
                )
                // Build the whole velk-ui tree. The runtime plugin loader
                // resolves plugin: URIs to lib<name>.so in the APK's lib/
                // dir at runtime, so every plugin .so has to be packaged
                // even though velk_ui_simple doesn't directly link them.
            }
        }

        ndk {
            abiFilters += listOf("arm64-v8a")
        }
    }

    externalNativeBuild {
        cmake {
            // Same CMakeLists as velk-platform: builds the full velk-ui tree
            // for Android. AGP places resulting .so files into the APK's
            // lib/<abi>/, where Android's linker can dlopen them by name.
            path = file("../../velk-platform/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

// Stage `samples/simple/scenes/` and `samples/simple/assets/` into
// <build>/staged_assets/{scenes,assets}/ so the APK's assets/ mirrors the
// desktop layout — `app://scenes/dashboard.json` resolves via
// AAssetManager_open("scenes/dashboard.json").
val stagedAssetsDir: Provider<Directory> = layout.buildDirectory.dir("staged_assets")

// Materialise the dir at configuration time so addStaticSourceDirectory below
// can register it without AGP complaining that it doesn't exist yet. The
// Sync task populates it before mergeAssets runs (wired via task dependency).
stagedAssetsDir.get().asFile.mkdirs()

val stageAssets by tasks.registering(Sync::class) {
    into(stagedAssetsDir)
    from("../../../samples/simple/scenes") { into("scenes") }
    from("../../../samples/simple/assets") { into("assets") }
}

androidComponents {
    onVariants { variant ->
        variant.sources.assets?.addStaticSourceDirectory(
            stagedAssetsDir.get().asFile.absolutePath
        )
    }
}

tasks.matching { it.name.startsWith("merge") && it.name.endsWith("Assets") }
    .configureEach { dependsOn(stageAssets) }
