plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.harman.usbcam"
    compileSdk = 34
    defaultConfig {
        applicationId = "com.harman.usbcam"
        minSdk = 26
        targetSdk = 34
        // set by the GitHub Actions workflow (-PappVersionCode=<run number>) so every build is newer than the one before
        versionCode = (project.findProperty("appVersionCode") as String?)?.toIntOrNull() ?: 6
        versionName = (project.findProperty("appVersionName") as String?) ?: "0.7"
    }
    // Fixed debug key (checked into the repo, not a secret): every build is signed the same, so updates install over the old app
    // and keep the camera permission. (The first install after this change needs one uninstall; setup.ps1 does it.)
    signingConfigs { getByName("debug") { storeFile = file("debug.keystore"); storePassword = "android"; keyAlias = "androiddebugkey"; keyPassword = "android" } }
    buildTypes { release { isMinifyEnabled = false } }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}
dependencies {
    // Material Components (Material 3 theme + widgets) on plain Views: lowest-risk route to M3, no Compose compiler needed.
    implementation("com.google.android.material:material:1.12.0")
    implementation("androidx.appcompat:appcompat:1.7.0")
}
