// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.utils

import androidx.preference.PreferenceManager
import java.io.File
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.features.settings.model.StringSetting

/**
 * Eden UMA ships its Turnip build (VK_EXT_external_memory_host on KGSL) inside the APK so that
 * host aliasing works without a manual driver install. The zip is copied into the driver list
 * after every app update and selected once on GPUs it supports. Later choices made in the driver
 * manager are left alone.
 */
object BundledDriver {
    private const val ASSET_PATH = "eden-uma/turnip-uma.zip"
    private const val FILE_NAME = "eden-uma-turnip.zip"
    private const val PREF_COPIED_VERSION = "eden_uma_bundled_driver_version"
    private const val PREF_AUTO_SELECTED = "eden_uma_bundled_driver_selected"

    // Adreno 8xx parts listed by the bundled Turnip's freedreno_devices.py
    private val supportedModels = setOf(810, 812, 825, 829, 830, 840)

    val driverFile: File get() = File(GpuDriverHelper.driverStoragePath + FILE_NAME)

    fun install() {
        val context = YuzuApplication.appContext
        val preferences = PreferenceManager.getDefaultSharedPreferences(context)
        try {
            val version = context.packageManager
                .getPackageInfo(context.packageName, 0).longVersionCode
            if (preferences.getLong(PREF_COPIED_VERSION, -1) != version || !driverFile.exists()) {
                GpuDriverHelper.initializeDirectories()
                context.assets.open(ASSET_PATH).use { input ->
                    driverFile.outputStream().use { input.copyTo(it) }
                }
                preferences.edit().putLong(PREF_COPIED_VERSION, version).apply()
            }

            if (preferences.getBoolean(PREF_AUTO_SELECTED, false) || !isSupportedGpu()) {
                return
            }
            if (StringSetting.DRIVER_PATH.getString(needsGlobal = true).isEmpty()) {
                StringSetting.DRIVER_PATH.setString(driverFile.path)
                NativeConfig.saveGlobalConfig()
                Log.info("[BundledDriver] Selected the bundled Turnip driver")
            }
            preferences.edit().putBoolean(PREF_AUTO_SELECTED, true).apply()
        } catch (e: Exception) {
            Log.warning("[BundledDriver] Could not install the bundled driver: ${e.message}")
        }
    }

    private fun isSupportedGpu(): Boolean {
        if (!GpuDriverHelper.isAdrenoGpu()) {
            return false
        }
        val hookLibPath = GpuDriverHelper.hookLibPath ?: return false
        // format: Adreno (TM) <ModelNumber>
        val parts = GpuDriverHelper.getGpuModel(hookLibPath = hookLibPath)?.split(" ")
            ?: return false
        if (parts.size < 3 || parts[0] != "Adreno") {
            return false
        }
        return parts[2].toIntOrNull() in supportedModels
    }
}
