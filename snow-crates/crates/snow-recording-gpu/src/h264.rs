//! H.264 bitstream helpers: Annex-B NAL splitting and avcC construction.
//!
//! The Media Foundation encoder emits an Annex-B byte stream (NAL units
//! separated by start codes) and reports the sequence header as an
//! Annex-B blob. MP4 muxing needs an avcC record (length-prefixed SPS and
//! PPS); these helpers convert between the two forms. Everything here is
//! pure byte manipulation and unit-testable without a GPU.

/// H.264 NAL unit type numbers of interest here.
#[allow(dead_code)]
pub const NAL_TYPE_SLICE: u8 = 1;
#[allow(dead_code)]
pub const NAL_TYPE_IDR: u8 = 5;
#[allow(dead_code)]
pub const NAL_TYPE_SEI: u8 = 6;
pub const NAL_TYPE_SPS: u8 = 7;
pub const NAL_TYPE_PPS: u8 = 8;

/// Split an Annex-B byte stream into NAL units (without start codes).
pub fn split_annexb_nals(data: &[u8]) -> Vec<&[u8]> {
    let mut nals = Vec::new();
    let mut starts: Vec<(usize, usize)> = Vec::new(); // (nal_start, next_scan)
    let mut index = 0usize;
    while index + 3 <= data.len() {
        if data[index] == 0 && data[index + 1] == 0 {
            if data[index + 2] == 1 {
                starts.push((index + 3, index + 3));
                index += 3;
                continue;
            }
            if data[index + 2] == 0 && index + 4 <= data.len() && data[index + 3] == 1 {
                starts.push((index + 4, index + 4));
                index += 4;
                continue;
            }
        }
        index += 1;
    }
    for (position, (nal_start, _)) in starts.iter().enumerate() {
        let next_nal = starts
            .get(position + 1)
            .map(|(next, _)| {
                let next = *next;
                // Strip the start code that precedes the next NAL; check
                // the 4-byte form first, since its tail is also a valid
                // 3-byte code.
                let four_byte = next >= 4
                    && data[next - 4] == 0
                    && data[next - 3] == 0
                    && data[next - 2] == 0
                    && data[next - 1] == 1;
                if four_byte { next - 4 } else { next - 3 }
            })
            .unwrap_or(data.len());
        if next_nal > *nal_start {
            nals.push(&data[*nal_start..next_nal]);
        }
    }
    nals
}

/// NAL unit type (low 5 bits of the first header byte).
pub fn nal_type(nal: &[u8]) -> Option<u8> {
    nal.first().map(|header| header & 0x1F)
}

fn is_nal_type(nal: &[u8], wanted: u8) -> bool {
    nal_type(nal) == Some(wanted) && nal.len() >= 2
}

/// Build an avcC record from an Annex-B sequence header blob.
///
/// Extracts the SPS and PPS NAL units and emits the AVCDecoderConfiguration
/// record with 4-byte NAL length prefixes. Returns an empty vector when
/// the blob contains neither SPS nor PPS.
pub fn build_avcc_from_annexb(sequence_header: &[u8]) -> Vec<u8> {
    let nals = split_annexb_nals(sequence_header);
    let sps: Vec<&[u8]> = nals
        .iter()
        .copied()
        .filter(|nal| is_nal_type(nal, NAL_TYPE_SPS))
        .collect();
    let pps: Vec<&[u8]> = nals
        .iter()
        .copied()
        .filter(|nal| is_nal_type(nal, NAL_TYPE_PPS))
        .collect();
    if sps.is_empty() || pps.is_empty() {
        return Vec::new();
    }
    let (profile, compat, level) = match sps[0] {
        [_, profile, compat, level, ..] => (*profile, *compat, *level),
        _ => return Vec::new(),
    };
    let mut record = Vec::with_capacity(
        8 + sps.iter().map(|nal| nal.len() + 2).sum::<usize>()
            + pps.iter().map(|nal| nal.len() + 2).sum::<usize>(),
    );
    record.extend_from_slice(&[
        0x01, // configuration version
        profile,
        compat,
        level,
        0xFF,                   // lengthSizeMinusOne = 3 (4-byte prefixes)
        0xE0 | sps.len() as u8, // complete set of SPS
    ]);
    for nal in sps {
        record.extend_from_slice(&(nal.len() as u16).to_be_bytes());
        record.extend_from_slice(nal);
    }
    record.push(pps.len() as u8);
    for nal in pps {
        record.extend_from_slice(&(nal.len() as u16).to_be_bytes());
        record.extend_from_slice(nal);
    }
    record
}

/// Prepend Annex-B start codes and NAL units (e.g. SPS/PPS) to a packet so
/// every keyframe is self-describing.
pub fn prepend_nals(packet: &mut Vec<u8>, nals: &[&[u8]]) {
    let mut prefix = Vec::new();
    for nal in nals {
        prefix.extend_from_slice(&[0, 0, 0, 1]);
        prefix.extend_from_slice(nal);
    }
    let suffix = std::mem::take(packet);
    packet.extend_from_slice(&prefix);
    packet.extend_from_slice(&suffix);
}

/// Pick SPS/PPS NALs from a sequence header, for keyframe prepending.
pub fn parameter_sets(sequence_header: &[u8]) -> Vec<&[u8]> {
    split_annexb_nals(sequence_header)
        .into_iter()
        .filter(|nal| is_nal_type(nal, NAL_TYPE_SPS) || is_nal_type(nal, NAL_TYPE_PPS))
        .collect()
}

/// Whether a packet contains an IDR slice NAL. Hardware encoders do not
/// reliably set the Media Foundation clean-point attribute, so keyframes
/// are detected from the bitstream.
pub fn packet_starts_with_idr(packet: &[u8]) -> bool {
    split_annexb_nals(packet)
        .iter()
        .any(|nal| nal_type(nal) == Some(NAL_TYPE_IDR))
}

/// Map milliseconds onto the 100 ns units Media Foundation uses.
pub fn ms_to_hns(timestamp_ms: u64) -> i64 {
    (timestamp_ms.saturating_mul(10_000)).min(i64::MAX as u64) as i64
}

/// Map Media Foundation 100 ns units back to milliseconds.
pub fn hns_to_ms(timestamp_hns: i64) -> u64 {
    (timestamp_hns.max(0) / 10_000) as u64
}

#[cfg(test)]
mod tests {
    use super::*;

    const START_CODE: &[u8] = &[0, 0, 0, 1];

    fn nal(unit_type: u8, payload: &[u8]) -> Vec<u8> {
        let mut bytes = Vec::new();
        bytes.extend_from_slice(START_CODE);
        // forbidden_zero(0) | nal_ref_idc(3) | unit_type(5).
        bytes.push(0x60 | unit_type);
        bytes.extend_from_slice(payload);
        bytes
    }

    #[test]
    fn splits_three_and_four_byte_start_codes() {
        let sps = nal(NAL_TYPE_SPS, &[0x64, 0x00, 0x1f, 0xac]);
        let pps = nal(NAL_TYPE_PPS, &[0xee]);
        let idr = {
            let mut bytes = Vec::new();
            bytes.extend_from_slice(&[0, 0, 1]); // three-byte start code
            bytes.push(0x60 | NAL_TYPE_IDR);
            bytes.extend_from_slice(&[9, 9, 9]);
            bytes
        };
        let mut stream = Vec::new();
        stream.extend_from_slice(&sps);
        stream.extend_from_slice(&pps);
        stream.extend_from_slice(&idr);

        let nals = split_annexb_nals(&stream);
        assert_eq!(nals.len(), 3);
        assert_eq!(nal_type(nals[0]), Some(NAL_TYPE_SPS));
        assert_eq!(nal_type(nals[1]), Some(NAL_TYPE_PPS));
        assert_eq!(nal_type(nals[2]), Some(NAL_TYPE_IDR));
        assert_eq!(&nals[0][1..], &[0x64, 0x00, 0x1f, 0xac]);
    }

    #[test]
    fn splitting_ignores_empty_nals_and_trailing_garbage() {
        let mut stream = Vec::new();
        stream.extend_from_slice(&nal(NAL_TYPE_SPS, &[1]));
        stream.extend_from_slice(&[0, 0, 0, 1]); // empty NAL
        stream.extend_from_slice(&nal(NAL_TYPE_PPS, &[2]));
        stream.extend_from_slice(&[0, 0]); // trailing zero bytes
        let nals = split_annexb_nals(&stream);
        assert_eq!(nals.len(), 2);
        assert_eq!(nal_type(nals[0]), Some(NAL_TYPE_SPS));
        assert_eq!(nal_type(nals[1]), Some(NAL_TYPE_PPS));
    }

    #[test]
    fn avcc_record_carries_sps_and_pps_with_length_prefixes() {
        let sps = [0x64u8, 0x00, 0x1f, 0xac];
        let pps = [0xeeu8, 0x3c];
        let mut sequence_header = Vec::new();
        sequence_header.extend_from_slice(&nal(NAL_TYPE_SPS, &sps));
        sequence_header.extend_from_slice(&nal(NAL_TYPE_PPS, &pps));

        let avcc = build_avcc_from_annexb(&sequence_header);
        assert!(!avcc.is_empty());
        assert_eq!(avcc[0], 0x01);
        assert_eq!(avcc[1], 0x64); // profile copied from SPS
        assert_eq!(avcc[4], 0xFF); // 4-byte length prefixes
        assert_eq!(avcc[5], 0xE1); // one SPS
        let sps_len = u16::from_be_bytes([avcc[6], avcc[7]]) as usize;
        // NAL header byte + payload
        assert_eq!(sps_len, sps.len() + 1);
        assert_eq!(&avcc[8..8 + sps_len][1..], &sps);
        let pps_offset = 8 + sps_len;
        assert_eq!(avcc[pps_offset], 1); // one PPS
        let pps_len = u16::from_be_bytes([avcc[pps_offset + 1], avcc[pps_offset + 2]]) as usize;
        assert_eq!(pps_len, pps.len() + 1);
    }

    #[test]
    fn avcc_without_parameter_sets_is_empty() {
        let sequence_header = nal(NAL_TYPE_IDR, &[1, 2, 3]);
        assert!(build_avcc_from_annexb(&sequence_header).is_empty());
        assert!(build_avcc_from_annexb(&[]).is_empty());
    }

    #[test]
    fn parameter_sets_prepend_before_packet_bytes() {
        let sps = nal(NAL_TYPE_SPS, &[1]);
        let mut sequence_header = sps.clone();
        sequence_header.extend_from_slice(&nal(NAL_TYPE_PPS, &[2]));
        let mut packet = nal(NAL_TYPE_IDR, &[7, 7]);
        prepend_nals(&mut packet, &parameter_sets(&sequence_header));
        let nals = split_annexb_nals(&packet);
        assert_eq!(
            nals.iter()
                .map(|nal| nal_type(nal).unwrap())
                .collect::<Vec<_>>(),
            vec![NAL_TYPE_SPS, NAL_TYPE_PPS, NAL_TYPE_IDR]
        );
        assert!(packet_starts_with_idr(&packet));
    }

    #[test]
    fn non_idr_packets_are_detected() {
        assert!(!packet_starts_with_idr(&nal(NAL_TYPE_SLICE, &[1])));
        assert!(packet_starts_with_idr(&nal(NAL_TYPE_IDR, &[1])));
    }

    #[test]
    fn hns_conversions_round_trip() {
        assert_eq!(ms_to_hns(0), 0);
        assert_eq!(ms_to_hns(1), 10_000);
        assert_eq!(hns_to_ms(10_000), 1);
        assert_eq!(hns_to_ms(10_499), 1);
        assert_eq!(hns_to_ms(-5), 0);
        assert_eq!(hns_to_ms(ms_to_hns(u32::MAX as u64)), u32::MAX as u64);
    }
}
