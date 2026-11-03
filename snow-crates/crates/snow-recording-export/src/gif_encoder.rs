//! RGB differences are determined before quantization, so replacing a local
//! palette cannot invalidate unchanged pixels or turn a small update into a
//! complete image. FFmpeg still owns packet timing, looping and publication.

use std::borrow::Cow;

use ffmpeg_next as ffmpeg;

use crate::error::{RecordingExportError, Result};
use crate::gif_palette::GifPalette;

/// GIF's muxer consumes complete GIF packets and needs no opened FFmpeg encoder.
/// Describe their decoded canvas independently of the RGB input transport.
pub(crate) fn muxer_parameters(width: u32, height: u32) -> ffmpeg::codec::Parameters {
    let mut parameters = ffmpeg::codec::Parameters::new();
    parameters.set_medium(ffmpeg::media::Type::Video);
    parameters.set_id(ffmpeg::codec::Id::GIF);
    // SAFETY: parameters owns this newly allocated codec parameter block.
    unsafe {
        let raw = parameters.as_mut_ptr();
        (*raw).width = width as i32;
        (*raw).height = height as i32;
        (*raw).format = ffmpeg::ffi::AVPixelFormat::AV_PIX_FMT_PAL8 as i32;
    }
    parameters
}

pub(crate) struct GifEncoder {
    palette: GifPalette,
    previous_rgb: Vec<u8>,
    indexed: ffmpeg::frame::Video,
    writer: Option<gif::Encoder<Vec<u8>>>,
    global_palette: Vec<u8>,
}

impl GifEncoder {
    pub(crate) fn new(width: u32, height: u32, quality: u8) -> Result<Self> {
        if width == 0 || height == 0 || width > u32::from(u16::MAX) || height > u32::from(u16::MAX)
        {
            return Err(RecordingExportError::InvalidConfig(
                "GIF dimensions must fit in 16 bits".into(),
            ));
        }
        Ok(Self {
            palette: GifPalette::new(quality),
            previous_rgb: Vec::new(),
            indexed: ffmpeg::frame::Video::new(ffmpeg::format::Pixel::PAL8, width, height),
            writer: None,
            global_palette: Vec::new(),
        })
    }

    pub(crate) fn encode(&mut self, rgb: &ffmpeg::frame::Video) -> Result<ffmpeg::Packet> {
        self.palette
            .convert(rgb, &mut self.indexed, &self.previous_rgb);
        let width = rgb.width() as usize;
        let height = rgb.height() as usize;
        let mut left = width;
        let mut top = height;
        let mut right = 0;
        let mut bottom = 0;
        for y in 0..height {
            let row = &self.indexed.data(0)[y * self.indexed.stride(0)..][..width];
            for (x, &index) in row.iter().enumerate() {
                if index != 255 {
                    left = left.min(x);
                    top = top.min(y);
                    right = right.max(x + 1);
                    bottom = bottom.max(y + 1);
                }
            }
        }
        // A repeated observation still needs its timing when callers preserve
        // every PTS. A transparent 1x1 image leaves the composed canvas intact.
        if right == 0 {
            left = 0;
            top = 0;
            right = 1;
            bottom = 1;
        }
        let palette = self.palette.rgb_palette();
        let transparent = (palette.len() / 3 - 1) as u8;
        let mut pixels = Vec::with_capacity((right - left) * (bottom - top));
        for y in top..bottom {
            pixels.extend(
                self.indexed.data(0)[y * self.indexed.stride(0) + left..][..right - left]
                    .iter()
                    .map(|&index| if index == 255 { transparent } else { index }),
            );
        }
        if self.writer.is_none() {
            self.writer = Some(
                gif::Encoder::new(Vec::new(), width as u16, height as u16, &palette)
                    .map_err(encode_error)?,
            );
            self.global_palette = palette.clone();
        }
        let frame = gif::Frame {
            left: left as u16,
            top: top as u16,
            width: (right - left) as u16,
            height: (bottom - top) as u16,
            dispose: gif::DisposalMethod::Keep,
            transparent: Some(transparent),
            palette: (palette != self.global_palette).then_some(palette),
            buffer: Cow::Owned(pixels),
            ..gif::Frame::default()
        };
        let writer = self.writer.as_mut().expect("initialized GIF writer");
        writer.write_frame(&frame).map_err(encode_error)?;
        // Each packet starts with a GCE; the first also carries the GIF header.
        // The muxer inserts delays and the loop extension and writes the trailer.
        let packet = ffmpeg::Packet::copy(writer.get_ref());
        writer.get_mut().clear();
        self.previous_rgb.resize(width * height * 3, 0);
        for y in 0..height {
            self.previous_rgb[y * width * 3..][..width * 3]
                .copy_from_slice(&rgb.data(0)[y * rgb.stride(0)..][..width * 3]);
        }
        Ok(packet)
    }
}

fn encode_error(error: gif::EncodingError) -> RecordingExportError {
    RecordingExportError::Encode(format!("GIF frame encoding: {error}"))
}
