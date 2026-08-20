const FRUIT_ALIASES: &[(&str, &str)] = &[
    ("mangocarabao", "Mango Carabao"),
    ("carabaomango", "Mango Carabao"),
    ("carabao", "Mango Carabao"),
    ("philippinemango", "Mango Carabao"),
    ("manilamango", "Mango Carabao"),
    ("indianmango", "Indian Mango"),
    ("mangoindian", "Indian Mango"),
    ("applemango", "Apple Mango"),
    ("mangoapple", "Apple Mango"),
    ("mango", "Unknown"),
    ("apple", "Apple"),
    ("dragonfruit", "Dragon Fruit"),
    ("pitaya", "Dragon Fruit"),
    ("pitahaya", "Dragon Fruit"),
    ("watermelon", "Watermelon"),
    ("pineapple", "Pineapple"),
    ("mangosteen", "Mangosteen"),
    ("banana", "Banana"),
    ("avocado", "Avocado"),
    ("grapes", "Grapes"),
    ("grape", "Grapes"),
    ("durian", "Durian"),
    ("pomelo", "Pomelo"),
    ("pummelo", "Pomelo"),
    ("pear", "Pear"),
    ("lemon", "Lemon"),
    ("lime", "Lemon"),
    ("orange", "Orange"),
    ("mandarin", "Orange"),
    ("tangerine", "Orange"),
    ("clementine", "Orange"),
];

pub fn canonical_fruit(raw: &str) -> &'static str {
    let normalized: String = raw
        .chars()
        .filter(|c| c.is_ascii_alphanumeric())
        .map(|c| c.to_ascii_lowercase())
        .collect();

    let mut best: Option<(usize, &'static str)> = None;
    for (alias, label) in FRUIT_ALIASES {
        if normalized.contains(alias) && best.is_none_or(|(len, _)| alias.len() > len) {
            best = Some((alias.len(), label));
        }
    }
    best.map(|(_, label)| label).unwrap_or("Unknown")
}

#[cfg(test)]
mod tests {
    use super::canonical_fruit;

    #[test]
    fn bare_mango_is_rejected_as_unknown() {
        assert_eq!(canonical_fruit("Mango."), "Unknown");
    }

    #[test]
    fn variety_wins_over_the_bare_mango_alias() {
        assert_eq!(canonical_fruit("Carabao mango"), "Mango Carabao");
    }

    #[test]
    fn label_is_found_inside_a_spoken_sentence() {
        assert_eq!(
            canonical_fruit("The fruit is a Mango Carabao."),
            "Mango Carabao"
        );
    }

    #[test]
    fn close_relatives_collapse_into_the_list() {
        assert_eq!(canonical_fruit("Tangerine"), "Orange");
    }

    #[test]
    fn empty_transcript_is_unknown() {
        assert_eq!(canonical_fruit(""), "Unknown");
    }

    #[test]
    fn fruit_outside_the_list_is_unknown() {
        assert_eq!(canonical_fruit("Kiwi"), "Unknown");
    }
}
