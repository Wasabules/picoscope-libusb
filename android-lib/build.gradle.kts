plugins {
    id("com.android.library")
    id("maven-publish")
}

android {
    namespace = "io.github.wasabules.ps2204"
    compileSdk = 34
    ndkVersion = "26.1.10909125"

    defaultConfig {
        minSdk = 24

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_static"
                )
            }
        }

        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
        }

        consumerProguardFiles("consumer-rules.pro")
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    publishing {
        singleVariant("release") {
            withSourcesJar()
        }
    }
}

// JitPack publication — JitPack looks for a release variant and an
// artifactId matching the repo name.
afterEvaluate {
    publishing {
        publications {
            create<MavenPublication>("release") {
                from(components["release"])
                groupId = "com.github.Wasabules"
                artifactId = "picoscope-libusb"
                version = project.findProperty("VERSION_NAME")?.toString() ?: "0.1.0"

                pom {
                    name.set("picoscope-libusb (Android)")
                    description.set(
                        "Reverse-engineered libusb driver + JNI bindings for " +
                        "the PicoScope 2204A USB oscilloscope. Not affiliated " +
                        "with Pico Technology Ltd."
                    )
                    url.set("https://github.com/Wasabules/picoscope-libusb")
                    licenses {
                        license {
                            name.set("MIT")
                            url.set("https://opensource.org/licenses/MIT")
                        }
                    }
                }
            }
        }
    }
}
