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
        versionCode = 1
        versionName = "0.1-m1g"
    }
    buildTypes { release { isMinifyEnabled = false } }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}
// No dependencies on purpose: keeps APK tiny and build fast.
