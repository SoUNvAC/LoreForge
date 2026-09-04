ALTER TABLE blocks ADD COLUMN extraction_confidence REAL
    CHECK (
        extraction_confidence IS NULL OR
        (extraction_confidence >= 0.0 AND extraction_confidence <= 1.0)
    );
