SET NAMES utf8mb4;
SET FOREIGN_KEY_CHECKS = 0;

-- ----------------------------
-- Table structure for tb_heartbeat
-- ----------------------------
DROP TABLE IF EXISTS `tb_heartbeat`;
CREATE TABLE IF NOT EXISTS `tb_heartbeat` (
  `p_id` varchar(128) NOT NULL,
  `p_ts` timestamp(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  PRIMARY KEY (`p_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

SET FOREIGN_KEY_CHECKS = 1;
