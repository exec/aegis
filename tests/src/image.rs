//! Minimal PPM loader + fuzzy image comparison for screendump tests.
//!
//! QEMU's `screendump` emits a P6 PPM (binary RGB, 8-bit per channel,
//! 255 maxval). We parse the header by hand to avoid pulling in
//! the `image` crate — the format is tiny and deterministic.
//!
//! Comparison is fuzzy on purpose: exact byte equality is too brittle
//! for animated UI elements (focus-ring highlights, cursor blink).
//! We allow a small mean absolute pixel diff and a larger cap on the
//! number of wildly-different pixels.

use std::fs;
use std::path::Path;

pub struct Ppm {
    pub width: u32,
    pub height: u32,
    /// RGB bytes, row-major, width*height*3 total.
    pub pixels: Vec<u8>,
}

/// Parse a P6 PPM file. Returns Err(String) on any malformed header
/// or unexpected maxval.
pub fn load_ppm(path: &Path) -> Result<Ppm, String> {
    let bytes = fs::read(path).map_err(|e| format!("read {}: {}", path.display(), e))?;

    let mut cursor = 0usize;
    let read_token = |b: &[u8], c: &mut usize| -> Result<String, String> {
        // Skip whitespace and comments
        while *c < b.len() {
            match b[*c] {
                b' ' | b'\t' | b'\r' | b'\n' => *c += 1,
                b'#' => {
                    while *c < b.len() && b[*c] != b'\n' {
                        *c += 1;
                    }
                }
                _ => break,
            }
        }
        let start = *c;
        while *c < b.len() && !matches!(b[*c], b' ' | b'\t' | b'\r' | b'\n') {
            *c += 1;
        }
        if start == *c {
            return Err("unexpected end of PPM header".into());
        }
        std::str::from_utf8(&b[start..*c])
            .map(str::to_string)
            .map_err(|e| format!("non-ASCII in PPM header: {}", e))
    };

    let magic = read_token(&bytes, &mut cursor)?;
    if magic != "P6" {
        return Err(format!("expected P6 magic, got {:?}", magic));
    }
    let width: u32 = read_token(&bytes, &mut cursor)?
        .parse()
        .map_err(|e| format!("bad width: {}", e))?;
    let height: u32 = read_token(&bytes, &mut cursor)?
        .parse()
        .map_err(|e| format!("bad height: {}", e))?;
    let maxval: u32 = read_token(&bytes, &mut cursor)?
        .parse()
        .map_err(|e| format!("bad maxval: {}", e))?;
    if maxval != 255 {
        return Err(format!("only maxval=255 supported, got {}", maxval));
    }
    // Single whitespace byte separates header from pixel data.
    if cursor >= bytes.len() {
        return Err("PPM truncated at header/data boundary".into());
    }
    cursor += 1;

    let expected = (width as usize) * (height as usize) * 3;
    if bytes.len() - cursor < expected {
        return Err(format!(
            "PPM pixel data truncated: have {} bytes, need {}",
            bytes.len() - cursor,
            expected
        ));
    }
    let pixels = bytes[cursor..cursor + expected].to_vec();
    Ok(Ppm { width, height, pixels })
}

/// Result of comparing two PPMs.
pub struct PpmDiff {
    pub mean_abs_diff: f64,
    /// Pixels where any channel differs by more than 24 (≈10% of 255).
    pub bad_pixels: usize,
    pub total_pixels: usize,
}

pub fn compare_ppm(a: &Ppm, b: &Ppm) -> Result<PpmDiff, String> {
    if a.width != b.width || a.height != b.height {
        return Err(format!(
            "dimension mismatch: {}x{} vs {}x{}",
            a.width, a.height, b.width, b.height
        ));
    }
    let mut total_diff: u64 = 0;
    let mut bad_pixels: usize = 0;
    let total_pixels = (a.width as usize) * (a.height as usize);
    for i in 0..total_pixels {
        let j = i * 3;
        let dr = (a.pixels[j] as i32 - b.pixels[j] as i32).unsigned_abs();
        let dg = (a.pixels[j + 1] as i32 - b.pixels[j + 1] as i32).unsigned_abs();
        let db = (a.pixels[j + 2] as i32 - b.pixels[j + 2] as i32).unsigned_abs();
        total_diff += (dr + dg + db) as u64;
        if dr.max(dg).max(db) > 24 {
            bad_pixels += 1;
        }
    }
    let mean_abs_diff = total_diff as f64 / (total_pixels as f64 * 3.0);
    Ok(PpmDiff {
        mean_abs_diff,
        bad_pixels,
        total_pixels,
    })
}

/// Assert `actual` matches `reference` within fuzz tolerances.
///
/// Panics with a diagnostic message if mean absolute pixel diff exceeds
/// 2.0 (across all channels) OR more than 0.5% of pixels are wildly
/// different (any channel delta > 24).
pub fn assert_ppm_matches(actual: &Path, reference: &Path) {
    let a = load_ppm(actual).unwrap_or_else(|e| panic!("load actual: {}", e));
    let r = load_ppm(reference).unwrap_or_else(|e| panic!("load reference: {}", e));
    let diff = compare_ppm(&a, &r).unwrap_or_else(|e| panic!("compare: {}", e));

    let bad_ratio = diff.bad_pixels as f64 / diff.total_pixels as f64;
    let pass = diff.mean_abs_diff < 2.0 && bad_ratio < 0.005;
    assert!(
        pass,
        "screendump does not match reference:\n  actual:    {}\n  reference: {}\n  mean abs diff: {:.3} (threshold 2.0)\n  bad pixels:    {} / {} ({:.3}% , threshold 0.5%)",
        actual.display(),
        reference.display(),
        diff.mean_abs_diff,
        diff.bad_pixels,
        diff.total_pixels,
        bad_ratio * 100.0,
    );
}

#[cfg(test)]
mod tests {
    use super::*;

    fn make_ppm(w: u32, h: u32, fill: [u8; 3]) -> Ppm {
        let mut pixels = Vec::with_capacity((w * h * 3) as usize);
        for _ in 0..(w * h) {
            pixels.extend_from_slice(&fill);
        }
        Ppm { width: w, height: h, pixels }
    }

    #[test]
    fn compare_identical() {
        let a = make_ppm(10, 10, [128, 128, 128]);
        let b = make_ppm(10, 10, [128, 128, 128]);
        let d = compare_ppm(&a, &b).unwrap();
        assert_eq!(d.mean_abs_diff, 0.0);
        assert_eq!(d.bad_pixels, 0);
    }

    #[test]
    fn compare_small_diff() {
        let a = make_ppm(10, 10, [128, 128, 128]);
        let b = make_ppm(10, 10, [130, 130, 130]);
        let d = compare_ppm(&a, &b).unwrap();
        assert!(d.mean_abs_diff > 1.9 && d.mean_abs_diff < 2.1);
        assert_eq!(d.bad_pixels, 0);
    }

    #[test]
    fn compare_big_diff() {
        let a = make_ppm(10, 10, [0, 0, 0]);
        let b = make_ppm(10, 10, [255, 255, 255]);
        let d = compare_ppm(&a, &b).unwrap();
        assert_eq!(d.mean_abs_diff, 255.0);
        assert_eq!(d.bad_pixels, 100);
    }

    #[test]
    fn compare_dimension_mismatch() {
        let a = make_ppm(10, 10, [0, 0, 0]);
        let b = make_ppm(20, 10, [0, 0, 0]);
        assert!(compare_ppm(&a, &b).is_err());
    }

    #[test]
    fn load_writes_roundtrip() {
        let tmp = std::env::temp_dir().join("aegis-image-test.ppm");
        let mut data = b"P6\n2 1\n255\n".to_vec();
        data.extend_from_slice(&[255, 0, 0, 0, 255, 0]);
        std::fs::write(&tmp, &data).unwrap();
        let p = load_ppm(&tmp).unwrap();
        assert_eq!(p.width, 2);
        assert_eq!(p.height, 1);
        assert_eq!(p.pixels, vec![255, 0, 0, 0, 255, 0]);
        let _ = std::fs::remove_file(&tmp);
    }
}
