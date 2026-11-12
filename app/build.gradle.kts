plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// -PprebuiltNative: package app/prebuilt/jniLibs instead of building with the NDK
// (used for on-device builds where the official NDK is not available).
val prebuiltNative = providers.gradleProperty("prebuiltNative").isPresent

android {
    namespace = "com.emunds.nds"
    compileSdk = 35
    // CI pins NDK 27.2; local builds can override with -Pemunds.ndkVersion=<installed version>
    ndkVersion = providers.gradleProperty("emunds.ndkVersion").getOrElse("27.2.12479018")

    defaultConfig {
        applicationId = "com.emunds.nds"
        minSdk = 26          // AAudio + GLES 3
        targetSdk = 35
        versionCode = 7
        versionName = "0.1.7"

        ndk {
            // The melonDS JIT targets AArch64 (and x86-64); 32-bit ARM would be interpreter-only and too slow.
            abiFilters += listOf("arm64-v8a")
        }
        if (!prebuiltNative) externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_static",
                    "-DCMAKE_BUILD_TYPE=Release",
                    "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON"
                )
            }
        }
    }

    if (prebuiltNative) {
        sourceSets["main"].jniLibs.srcDirs("prebuilt/jniLibs")
    } else {
        externalNativeBuild {
            cmake {
                path = file("src/main/cpp/CMakeLists.txt")
                version = "3.22.1"
            }
        }
    }

    signingConfigs {
        // CI passes a real keystore through env vars; otherwise fall back to the debug key
        create("release") {
            val ks = System.getenv("EMUNDS_KEYSTORE")
            if (ks != null && file(ks).exists()) {
                storeFile = file(ks)
                storePassword = System.getenv("EMUNDS_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("EMUNDS_KEY_ALIAS")
                keyPassword = System.getenv("EMUNDS_KEY_PASSWORD")
            } else {
                val dbg = signingConfigs.getByName("debug")
                storeFile = dbg.storeFile
                storePassword = dbg.storePassword
                keyAlias = dbg.keyAlias
                keyPassword = dbg.keyPassword
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            signingConfig = signingConfigs.getByName("release")
        }
        debug {
            // native code is always built optimized: a -O0 emulator is unusable
            if (!prebuiltNative) externalNativeBuild { cmake { arguments += "-DCMAKE_BUILD_TYPE=Release" } }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    buildFeatures {
        buildConfig = true
        viewBinding = true
    }

    packaging {
        jniLibs { useLegacyPackaging = false }
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.activity:activity-ktx:1.9.3")
    implementation("androidx.recyclerview:recyclerview:1.3.2")
    implementation("androidx.preference:preference-ktx:1.2.1")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.7")
    implementation("com.google.android.material:material:1.12.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
}
