// SPDX-License-Identifier: MIT
//! InstallShield 5 cabinet reader (`DATA1.HDR` + `DATA1.CAB`), as used on the German
//! Cultures Gold CD. Only major version 5 and files inside the first volume are
//! supported; other versions and spanned files are reported as unsupported.
//!
//! Compressed file data is a sequence of chunks, each a little-endian u16 length followed
//! by that many bytes of raw DEFLATE. Obfuscated files are de-obfuscated first.

use crate::{codepage, cstr, format_err, le16, le32, Archive, Entry, Error, ReadSeek, Result};
use flate2::{Decompress, FlushDecompress, Status};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::Path;

const SIGNATURE: u32 = 0x2863_5349; // "ISc("
const FLAG_OBFUSCATED: u16 = 0x0002;
const FLAG_COMPRESSED: u16 = 0x0004;
const FLAG_INVALID: u16 = 0x0008;

#[derive(Debug, Clone)]
struct CabFile {
    index: u32,
    flags: u16,
    expanded: u32,
    compressed: u32,
    data_offset: u32,
}

pub struct Cabinet {
    volume: Box<dyn ReadSeek>,
    entries: Vec<Entry>,
    files: Vec<CabFile>,
    first_index: u32,
    last_index: u32,
}

/// Major version from the common header's version field.
fn major_version(v: u32) -> u32 {
    match v >> 24 {
        1 => (v >> 12) & 0xf,
        2 | 4 => (v & 0xffff) / 100,
        _ => 0,
    }
}

impl Cabinet {
    /// `hdr` is the whole `DATA1.HDR`; `volume` reads `DATA1.CAB`.
    pub fn open(hdr: &[u8], mut volume: Box<dyn ReadSeek>) -> Result<Self> {
        if le32(hdr, 0)? != SIGNATURE {
            return format_err("not an InstallShield header (signature)");
        }
        let major = major_version(le32(hdr, 4)?);
        if major != 5 {
            return Err(Error::Unsupported(format!(
                "InstallShield cabinet version {major} (only 5)"
            )));
        }
        let b = le32(hdr, 0x0c)? as usize; // cab descriptor
        let t = b + le32(hdr, b + 0x0c)? as usize; // file table
        let dir_count = le32(hdr, b + 0x1c)? as usize;
        let file_count = le32(hdr, b + 0x28)? as usize;
        if dir_count > 100_000 || file_count > 1_000_000 {
            return format_err("implausible directory/file count");
        }
        let table = |i: usize| le32(hdr, t + 4 * i).map(|v| v as usize);

        let mut dirs = Vec::with_capacity(dir_count);
        for i in 0..dir_count {
            let name = codepage::cp1252(cstr(hdr, t + table(i)?)?);
            dirs.push(name.replace('\\', "/"));
        }

        let mut entries = Vec::new();
        let mut files = Vec::new();
        for i in 0..file_count {
            let d = t + table(dir_count + i)?;
            let name_off = le32(hdr, d)? as usize;
            let dir_index = usize::from(le16(hdr, d + 0x04)?);
            let flags = le16(hdr, d + 0x08)?;
            let expanded = le32(hdr, d + 0x0a)?;
            let compressed = le32(hdr, d + 0x0e)?;
            let data_offset = le32(hdr, d + 0x26)?;
            if flags & FLAG_INVALID != 0 || name_off == 0 || data_offset == 0 {
                continue;
            }
            let name = codepage::cp1252(cstr(hdr, t + name_off)?);
            let dir = dirs
                .get(dir_index)
                .ok_or_else(|| Error::Format(format!("file {name}: bad directory index")))?;
            let path = if dir.is_empty() {
                name
            } else {
                format!("{dir}/{name}")
            };
            entries.push(Entry {
                path,
                size: u64::from(expanded),
            });
            files.push(CabFile {
                index: i as u32,
                flags,
                expanded,
                compressed,
                data_offset,
            });
        }

        // Volume header (v5): first/last file index stored in this volume.
        let mut vh = [0u8; 0x3c];
        volume.seek(SeekFrom::Start(0))?;
        volume.read_exact(&mut vh)?;
        if le32(&vh, 0)? != SIGNATURE {
            return format_err("cabinet volume has no InstallShield signature");
        }
        Ok(Cabinet {
            volume,
            entries,
            files,
            first_index: le32(&vh, 0x1c)?,
            last_index: le32(&vh, 0x20)?,
        })
    }

    fn write_file(&mut self, k: usize, out: &mut dyn Write) -> Result<()> {
        let f = self.files[k].clone();
        let path = &self.entries[k].path;
        if f.index < self.first_index || f.index > self.last_index {
            return Err(Error::Unsupported(format!(
                "{path} is stored in another cabinet volume"
            )));
        }
        self.volume
            .seek(SeekFrom::Start(u64::from(f.data_offset)))?;
        let stored = if f.flags & FLAG_COMPRESSED != 0 {
            f.compressed
        } else {
            f.expanded
        };
        let mut raw = vec![0u8; stored as usize];
        self.volume.read_exact(&mut raw)?;
        if f.flags & FLAG_OBFUSCATED != 0 {
            deobfuscate(&mut raw);
        }
        if f.flags & FLAG_COMPRESSED == 0 {
            out.write_all(&raw)?;
            return Ok(());
        }
        let written = inflate_chunks(&raw, out)?;
        if written != u64::from(f.expanded) {
            return Err(Error::Checksum(format!(
                "{path}: {written} bytes, expected {}",
                f.expanded
            )));
        }
        Ok(())
    }
}

/// Inverse of InstallShield's byte obfuscation (counter starts at 0 per file).
fn deobfuscate(buf: &mut [u8]) {
    for (i, b) in buf.iter_mut().enumerate() {
        *b = (*b ^ 0xd5).rotate_right(2).wrapping_sub((i % 0x47) as u8);
    }
}

/// Decode `[u16 len][raw deflate]...` into `out`. Returns the number of bytes written.
fn inflate_chunks(mut data: &[u8], out: &mut dyn Write) -> Result<u64> {
    let mut total = 0u64;
    let mut buf = vec![0u8; 1 << 16];
    while !data.is_empty() {
        let len = usize::from(le16(data, 0)?);
        let chunk = data
            .get(2..2 + len)
            .ok_or_else(|| Error::Format("truncated compressed chunk".into()))?;
        data = &data[2 + len..];
        let mut z = Decompress::new(false);
        let mut input = chunk;
        loop {
            let before_in = z.total_in();
            let before_out = z.total_out();
            let status = z
                .decompress(input, &mut buf, FlushDecompress::Finish)
                .map_err(|e| Error::Format(format!("deflate: {e}")))?;
            let used = (z.total_in() - before_in) as usize;
            let produced = (z.total_out() - before_out) as usize;
            input = &input[used..];
            out.write_all(&buf[..produced])?;
            total += produced as u64;
            match status {
                Status::StreamEnd => break,
                _ if used == 0 && produced == 0 => {
                    if input.is_empty() {
                        break; // chunk ended without a final block marker
                    }
                    return format_err("deflate made no progress");
                }
                _ => {}
            }
        }
    }
    Ok(total)
}

impl Archive for Cabinet {
    fn entries(&self) -> &[Entry] {
        &self.entries
    }

    fn extract(&mut self, dest: &Path, select: &dyn Fn(&Entry) -> bool) -> Result<usize> {
        let mut n = 0;
        for k in 0..self.entries.len() {
            if !select(&self.entries[k]) {
                continue;
            }
            let path = self.entries[k].path.clone();
            let mut out = std::io::BufWriter::new(crate::create_output(dest, &path)?);
            self.write_file(k, &mut out)?;
            out.flush()?;
            n += 1;
        }
        Ok(n)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use flate2::{Compress, Compression, FlushCompress};

    #[test]
    fn version_decoding() {
        assert_eq!(major_version(0x0100_5201), 5); // Cultures Gold
        assert_eq!(major_version(0x0200_0258), 6);
        assert_eq!(major_version(0x0700_0000), 0);
    }

    #[test]
    fn obfuscation_round_trip() {
        let plain: Vec<u8> = (0..300u32).map(|i| (i * 7) as u8).collect();
        // Forward transform: (b + i%0x47) rotl 2, xor 0xd5.
        let mut enc: Vec<u8> = plain
            .iter()
            .enumerate()
            .map(|(i, &b)| b.wrapping_add((i % 0x47) as u8).rotate_left(2) ^ 0xd5)
            .collect();
        deobfuscate(&mut enc);
        assert_eq!(enc, plain);
    }

    #[test]
    fn chunked_raw_deflate() {
        let parts: [&[u8]; 2] = [b"first chunk, first chunk, first chunk", b"second"];
        let mut stream = Vec::new();
        for p in parts {
            let mut c = Compress::new(Compression::default(), false);
            let mut buf = vec![0u8; 256];
            c.compress(p, &mut buf, FlushCompress::Finish).unwrap();
            let n = c.total_out() as usize;
            stream.extend_from_slice(&(n as u16).to_le_bytes());
            stream.extend_from_slice(&buf[..n]);
        }
        let mut out = Vec::new();
        assert_eq!(inflate_chunks(&stream, &mut out).unwrap(), 43);
        assert_eq!(out, b"first chunk, first chunk, first chunksecond");
    }
}
