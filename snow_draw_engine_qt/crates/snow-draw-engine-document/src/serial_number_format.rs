use serde::{Deserialize, Serialize};

/// The notation of a sequence label, independent of its enclosing badge shape.
#[repr(u32)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub enum SerialNumberNumericType {
    #[default]
    Arabic = 0,
    Roman = 1,
    LowercaseLetters = 2,
    UppercaseLetters = 3,
    Chinese = 4,
}

pub fn format_serial_number(number: i64, numeric_type: SerialNumberNumericType) -> String {
    let number = number.max(0) as u64;
    match numeric_type {
        SerialNumberNumericType::Roman if (1..=3999).contains(&number) => {
            let mut remaining = number;
            let mut label = String::new();
            for (value, symbol) in [
                (1000, "M"),
                (900, "CM"),
                (500, "D"),
                (400, "CD"),
                (100, "C"),
                (90, "XC"),
                (50, "L"),
                (40, "XL"),
                (10, "X"),
                (9, "IX"),
                (5, "V"),
                (4, "IV"),
                (1, "I"),
            ] {
                while remaining >= value {
                    label.push_str(symbol);
                    remaining -= value;
                }
            }
            label
        }
        SerialNumberNumericType::LowercaseLetters | SerialNumberNumericType::UppercaseLetters
            if number > 0 =>
        {
            let base = if numeric_type == SerialNumberNumericType::LowercaseLetters {
                b'a'
            } else {
                b'A'
            };
            let mut remaining = number;
            let mut letters = Vec::new();
            while remaining > 0 {
                remaining -= 1;
                letters.push(char::from(base + (remaining % 26) as u8));
                remaining /= 26;
            }
            letters.into_iter().rev().collect()
        }
        SerialNumberNumericType::Chinese => chinese_number(number),
        _ => number.to_string(),
    }
}

fn chinese_number(mut number: u64) -> String {
    if number == 0 {
        return "零".to_owned();
    }
    const DIGITS: [char; 10] = ['零', '一', '二', '三', '四', '五', '六', '七', '八', '九'];
    const GROUP_UNITS: [&str; 5] = ["", "万", "亿", "兆", "京"];
    let mut groups = [0; 5];
    for group in &mut groups {
        *group = number % 10_000;
        number /= 10_000;
    }
    let mut label = String::new();
    let mut gap = false;
    for index in (0..groups.len()).rev() {
        let group = groups[index];
        if group == 0 {
            gap |= !label.is_empty();
            continue;
        }
        if !label.is_empty() && (gap || group < 1000) {
            label.push('零');
        }
        gap = false;
        let mut group_started = false;
        let mut zero_pending = false;
        for (place, unit) in [(1000, "千"), (100, "百"), (10, "十"), (1, "")] {
            let digit = (group / place) % 10;
            if digit == 0 {
                zero_pending |= group_started;
            } else {
                if zero_pending {
                    label.push('零');
                }
                label.push(DIGITS[digit as usize]);
                label.push_str(unit);
                group_started = true;
                zero_pending = false;
            }
        }
        label.push_str(GROUP_UNITS[index]);
    }
    // Only omit the leading one, never the one in a later group (一万零一十).
    if label.starts_with("一十") {
        label.drain(..'一'.len_utf8());
    }
    label
}

#[cfg(test)]
mod tests {
    use super::{SerialNumberNumericType::*, *};

    #[test]
    fn serial_number_formats_cover_boundaries() {
        for (kind, cases) in [
            (Arabic, vec![(0, "0"), (i64::MAX, "9223372036854775807")]),
            (
                Roman,
                vec![
                    (0, "0"),
                    (1, "I"),
                    (4, "IV"),
                    (9, "IX"),
                    (40, "XL"),
                    (90, "XC"),
                    (400, "CD"),
                    (900, "CM"),
                    (3999, "MMMCMXCIX"),
                    (4000, "4000"),
                    (i64::MAX, "9223372036854775807"),
                ],
            ),
            (
                LowercaseLetters,
                vec![
                    (0, "0"),
                    (1, "a"),
                    (26, "z"),
                    (27, "aa"),
                    (52, "az"),
                    (53, "ba"),
                    (702, "zz"),
                    (703, "aaa"),
                ],
            ),
            (
                UppercaseLetters,
                vec![
                    (0, "0"),
                    (1, "A"),
                    (26, "Z"),
                    (27, "AA"),
                    (702, "ZZ"),
                    (703, "AAA"),
                ],
            ),
            (
                Chinese,
                vec![
                    (0, "零"),
                    (1, "一"),
                    (10, "十"),
                    (11, "十一"),
                    (20, "二十"),
                    (101, "一百零一"),
                    (1001, "一千零一"),
                    (1010, "一千零一十"),
                    (10000, "一万"),
                    (10001, "一万零一"),
                    (10010, "一万零一十"),
                    (11000, "一万一千"),
                    (100000001, "一亿零一"),
                    (100010001, "一亿零一万零一"),
                    (1000000000000, "一兆"),
                    (10000000000000000, "一京"),
                    (
                        i64::MAX,
                        "九百二十二京三千三百七十二兆零三百六十八亿五千四百七十七万五千八百零七",
                    ),
                ],
            ),
        ] {
            for (number, expected) in cases {
                assert_eq!(
                    format_serial_number(number, kind),
                    expected,
                    "{kind:?}: {number}"
                );
            }
        }
    }

    #[test]
    fn serial_number_letters_preserve_the_maximum_integer() {
        for kind in [LowercaseLetters, UppercaseLetters] {
            let label = format_serial_number(i64::MAX, kind);
            let decoded = label.bytes().fold(0_u64, |value, letter| {
                value * 26 + u64::from(letter.to_ascii_lowercase() - b'a' + 1)
            });
            assert_eq!(decoded, i64::MAX as u64);
        }
    }

    #[test]
    fn serial_number_numeric_type_defaults_and_geometry_follow_the_label() {
        use crate::{SerialNumberData, resolve_serial_number_data_diameter};
        let mut serial = SerialNumberData {
            number: 888,
            ..SerialNumberData::default()
        };
        let arabic = resolve_serial_number_data_diameter(&serial, 0.0);
        serial.numeric_type = Roman;
        let roman = resolve_serial_number_data_diameter(&serial, 0.0);
        assert!(
            roman > arabic,
            "DCCCLXXXVIII must not use three-digit sizing"
        );
        serial.numeric_type = Chinese;
        let chinese = resolve_serial_number_data_diameter(&serial, 0.0);
        assert!(chinese > arabic);
        let encoded = serde_json::to_value(&serial).unwrap();
        assert_eq!(
            serde_json::from_value::<SerialNumberData>(encoded.clone()).unwrap(),
            serial
        );
        let mut legacy = encoded;
        legacy.as_object_mut().unwrap().remove("numeric_type");
        assert_eq!(
            serde_json::from_value::<SerialNumberData>(legacy)
                .unwrap()
                .numeric_type,
            Arabic
        );
    }
}
