UPDATE UnitPromotions SET PromotionPrereqOr1=PromotionPrereq, PromotionPrereq=NULL WHERE PromotionPrereq IS NOT NULL AND PromotionPrereqOr1 IS NULL;