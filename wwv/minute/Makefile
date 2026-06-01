WWV_CLIPS=v_0.raw v_1.raw v_10.raw v_11.raw v_12.raw v_13.raw v_14.raw v_15.raw v_16.raw v_17.raw v_18.raw v_19.raw v_2.raw v_20.raw v_21.raw v_22.raw v_23.raw v_24.raw v_25.raw v_26.raw v_27.raw v_28.raw v_29.raw v_3.raw v_30.raw v_31.raw v_32.raw v_33.raw v_34.raw v_35.raw v_36.raw v_37.raw v_38.raw v_39.raw v_4.raw v_40.raw v_41.raw v_42.raw v_43.raw v_44.raw v_45.raw v_46.raw v_47.raw v_48.raw v_49.raw v_5.raw v_50.raw v_51.raw v_52.raw v_53.raw v_54.raw v_55.raw v_56.raw v_57.raw v_58.raw v_59.raw v_6.raw v_7.raw v_8.raw v_9.raw v_at_the_tone.raw v_hour.raw v_hours.raw v_ident_better.raw v_ident.raw v_minute.raw v_minutes.raw v_utc.raw

.PHONY: all clean

all: $(WWV_CLIPS)

clean:
	rm -f $(WWV_CLIPS)


%.raw: %.mp3
	sox $< -t raw --rate 48000 --channels 1 --encoding signed-integer --bits 16 --endian little $@ 
