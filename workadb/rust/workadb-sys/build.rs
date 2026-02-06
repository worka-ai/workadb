use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

fn main() -> anyhow::Result<()> {
    println!("cargo:rerun-if-env-changed=WORKADB_DIST_DIR");
    println!("cargo:rerun-if-env-changed=WORKADB_ARTIFACTS_DIR");
    println!("cargo:rerun-if-env-changed=WORKADB_VERSION");
    println!("cargo:rerun-if-env-changed=WORKADB_ROOT");
    println!("cargo:rerun-if-env-changed=TARGET");

    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR")?);
    let repo_root = env::var("WORKADB_ROOT")
        .ok()
        .map(PathBuf::from)
        .or_else(|| find_repo_root(&manifest_dir))
        .ok_or_else(|| anyhow::anyhow!("failed to resolve workadb repo root"))?;

    let dist_override = env::var("WORKADB_DIST_DIR").ok().map(PathBuf::from);
    let artifacts_root = env::var("WORKADB_ARTIFACTS_DIR").ok().map(PathBuf::from);
    let workadb_version = env::var("WORKADB_VERSION").unwrap_or_else(|_| "0.1.0".to_string());

    let target = env::var("TARGET")?;
    let (platform, arch) = platform_arch_from_target(&target)?;

    let (mut lib_dir, mut include_dir) = if let Some(dist_dir) = dist_override.clone() {
        let lib = dist_dir.join("lib");
        let include = dist_dir.join("include");
        if !lib.join("libworkadb.a").exists() {
            return Err(anyhow::anyhow!(
                "libworkadb.a not found at {}",
                lib.display()
            ));
        }
        (lib, include)
    } else {
        let platform_tag = match platform {
            "macos" => "macos",
            "ios" => "ios",
            "ios-sim" => "ios-sim",
            "android" => "android",
            _ => platform,
        };

        if let Some(artifacts) = artifacts_root {
            let direct_lib = if artifacts.join("libworkadb.a").exists() {
                Some(artifacts.clone())
            } else if artifacts.join("lib").join("libworkadb.a").exists() {
                Some(artifacts.join("lib"))
            } else {
                None
            };
            if let Some(lib) = direct_lib {
                let include = if lib
                    .parent()
                    .map(|p| p.join("include"))
                    .map(|p| p.is_dir())
                    .unwrap_or(false)
                {
                    lib.parent().unwrap().join("include")
                } else {
                    lib.parent()
                        .and_then(|p| p.parent())
                        .map(|p| p.join("include"))
                        .unwrap_or_else(|| lib.parent().unwrap().join("include"))
                };
                (lib, include)
            } else {
                let base = artifacts
                    .join("workadb")
                    .join(&workadb_version)
                    .join(platform_tag)
                    .join(arch);
                (base.join("lib"), base.join("include"))
            }
        } else {
            let base = env::current_dir()
                .unwrap_or_else(|_| repo_root.clone())
                .join("artifacts")
                .join("workadb")
                .join(&workadb_version)
                .join(platform_tag)
                .join(arch);
            (base.join("lib"), base.join("include"))
        }
    };

    if dist_override.is_none() && !lib_dir.join("libworkadb.a").exists() {
        let dist_tag = match platform {
            "macos" => "desktop",
            "ios" => "ios",
            "ios-sim" => "ios-sim",
            "android" => "android",
            _ => platform,
        };
        let base = repo_root
            .join("workadb")
            .join("build")
            .join(format!("dist-{dist_tag}"))
            .join(arch);
        lib_dir = base.join("lib");
        include_dir = base.join("include");
    }

    if !lib_dir.join("libworkadb.a").exists() {
        return Err(anyhow::anyhow!(
            "libworkadb.a not found at {}",
            lib_dir.display()
        ));
    }

    let lib_path = lib_dir.join("libworkadb.a");
    println!("cargo:rerun-if-changed={}", lib_path.display());
    println!("cargo:rerun-if-changed={}", include_dir.display());
    println!(
        "cargo:warning=workadb-sys linking {} (include {})",
        lib_path.display(),
        include_dir.display()
    );

    println!("cargo:rustc-link-search=native={}", lib_dir.display());
    println!("cargo:rustc-link-lib=static=workadb");
    if target.contains("apple") {
        let archive = lib_dir.join("libworkadb.a");
        println!("cargo:rustc-link-arg=-Wl,-force_load,{}", archive.display());
        let linked = if platform == "macos" {
            link_icu_from_pkg_config() || link_icu_from_homebrew()
        } else {
            false
        };
        if !linked {
            println!("cargo:rustc-link-lib=icucore");
        }
    }
    println!("cargo:include={}", include_dir.display());

    Ok(())
}

fn platform_arch_from_target(target: &str) -> anyhow::Result<(&'static str, &'static str)> {
    let platform = if target.contains("apple-ios-sim") || target.contains("ios-sim") {
        "ios-sim"
    } else if target.contains("apple-ios") || target.ends_with("-ios") {
        "ios"
    } else if target.contains("apple-darwin") {
        "macos"
    } else if target.contains("android") {
        "android"
    } else if target.contains("linux") {
        "linux"
    } else {
        return Err(anyhow::anyhow!("unsupported target: {}", target));
    };

    let arch = if target.starts_with("aarch64") {
        match platform {
            "ios" | "ios-sim" | "macos" => "arm64",
            _ => "aarch64",
        }
    } else if target.starts_with("x86_64") {
        "x86_64"
    } else if target.starts_with("armv7") {
        "armv7"
    } else {
        return Err(anyhow::anyhow!("unsupported arch for target: {}", target));
    };

    Ok((platform, arch))
}

fn find_repo_root(start: &Path) -> Option<PathBuf> {
    let mut current = Some(start);
    while let Some(dir) = current {
        if dir.join("workadb").is_dir() && dir.join("src").is_dir() {
            return Some(dir.to_path_buf());
        }
        current = dir.parent();
    }
    None
}

fn link_icu_from_pkg_config() -> bool {
    let output = Command::new("pkg-config")
        .args(["--libs", "icu-uc", "icu-i18n"])
        .output();
    let Ok(output) = output else {
        return false;
    };
    if !output.status.success() {
        return false;
    }
    let libs = String::from_utf8_lossy(&output.stdout);
    let mut linked = false;
    for token in libs.split_whitespace() {
        if let Some(path) = token.strip_prefix("-L") {
            println!("cargo:rustc-link-search=native={}", path);
        } else if let Some(lib) = token.strip_prefix("-l") {
            if !lib.is_empty() {
                println!("cargo:rustc-link-lib=dylib={}", lib);
                linked = true;
            }
        }
    }
    linked
}

fn link_icu_from_homebrew() -> bool {
    let candidates = ["/opt/homebrew/opt/icu4c/lib", "/usr/local/opt/icu4c/lib"];
    for dir in candidates {
        let lib_path = Path::new(dir).join("libicuuc.dylib");
        if !lib_path.exists() {
            continue;
        }
        println!("cargo:rustc-link-search=native={}", dir);
        for lib in ["icui18n", "icuuc", "icudata"] {
            println!("cargo:rustc-link-lib=dylib={}", lib);
        }
        return true;
    }
    false
}
