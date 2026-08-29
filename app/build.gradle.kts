plugins {
    id("com.android.application")
}

val pluginVersionCode = providers.gradleProperty("pluginVersionCode")
    .orElse("1")
    .map(String::toInt)
val pluginVersionName = providers.gradleProperty("pluginVersionName")
    .orElse("development")
val releaseStoreFile = providers.environmentVariable("NAIVEFOX_SIGNING_STORE_FILE")
val releaseStorePassword = providers.environmentVariable("NAIVEFOX_SIGNING_STORE_PASSWORD")
val releaseKeyAlias = providers.environmentVariable("NAIVEFOX_SIGNING_KEY_ALIAS")
val releaseKeyPassword = providers.environmentVariable("NAIVEFOX_SIGNING_KEY_PASSWORD")

android {
    namespace = "com.github.incident201.naivefox.plugin"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.github.incident201.naivefox.plugin"
        minSdk = 26
        targetSdk = 36
        versionCode = pluginVersionCode.get()
        versionName = pluginVersionName.get()

        ndk {
            abiFilters += "arm64-v8a"
        }
    }

    signingConfigs {
        create("release") {
            storeFile = releaseStoreFile.orNull?.let { file(it) }
            storePassword = releaseStorePassword.orNull
            keyAlias = releaseKeyAlias.orNull
            keyPassword = releaseKeyPassword.orNull
        }
    }

    buildTypes {
        getByName("release") {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("release")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        buildConfig = false
    }

    packaging {
        jniLibs {
            useLegacyPackaging = true
            keepDebugSymbols += "**/libnaivefox_launcher.so"
        }
        resources.excludes += setOf(
            "META-INF/DEPENDENCIES",
            "META-INF/LICENSE*",
            "META-INF/NOTICE*",
        )
    }

    sourceSets.getByName("main") {
        assets.srcDir(rootProject.file("build/plugin-inputs/assets"))
        jniLibs.srcDir(rootProject.file("build/plugin-inputs/jniLibs"))
    }

    lint {
        abortOnError = true
        checkReleaseBuilds = true
        warningsAsErrors = true
        disable += setOf(
            // The project intentionally follows the Gradle/AGP pair used by NaiveProxy APKs.
            "AndroidGradlePluginVersion",
            // This plugin and its downloaded runtime are deliberately arm64-v8a-only.
            "ChromeOsAbiSupport",
        )
        htmlReport = false
        xmlReport = false
        textReport = true
    }
}
